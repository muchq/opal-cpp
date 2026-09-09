package io.smithycpp.codegen;

import java.util.Optional;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.LengthTrait;
import software.amazon.smithy.model.traits.PatternTrait;
import software.amazon.smithy.model.traits.RangeTrait;
import software.amazon.smithy.model.traits.SparseTrait;
import software.amazon.smithy.model.traits.Trait;
import software.amazon.smithy.model.traits.UniqueItemsTrait;

/**
 * Emits deterministic random-value builders for the generated integration tests: an {@code Rng}
 * helper plus one {@code Random<Shape>(Rng&)} function per aggregate shape. Values always satisfy
 * the model's constraint traits (the server validates before the handler runs) and stay inside the
 * subset that round-trips exactly on the wire: alphanumeric strings, whole-second timestamps,
 * dyadic-fraction floats, non-empty engaged containers, and non-empty blobs.
 */
final class RandomValueGenerator {

  private final CppContext context;

  RandomValueGenerator(CppContext context) {
    this.context = context;
  }

  private Shape target(MemberShape member) {
    return context.model().expectShape(member.getTarget());
  }

  private <T extends Trait> Optional<T> constraint(MemberShape member, Class<T> t) {
    Optional<T> onMember = member.getTrait(t);
    return onMember.isPresent() ? onMember : target(member).getTrait(t);
  }

  /** The shared Rng helper (deterministic; {@code fill_all} drives the maximal coverage rows). */
  static void writeRngStruct(CppWriter w) {
    w.addInclude("<cstddef>");
    w.addInclude("<cstdint>");
    w.addInclude("<random>");
    w.addInclude("<string>");
    w.write("// Deterministic pseudo-random source for round-trip inputs; fill_all forces");
    w.write("// every optional member on and maximum container sizes (the \"maximal\" rows).");
    w.openBlock("struct Rng {");
    w.write("std::mt19937 engine;");
    w.write("bool fill_all = false;");
    w.write("");
    w.write("bool Coin() { return fill_all || (engine() & 1U) != 0; }");
    w.write("");
    w.openBlock("std::size_t Size(std::size_t min_size, std::size_t max_size) {");
    w.write("if (fill_all || max_size <= min_size) return max_size;");
    w.write("return min_size + engine() % (max_size - min_size + 1);");
    w.closeBlock("}");
    w.write("");
    w.write("// Inclusive and boundary-biased: min/max come up 10% of the time each.");
    w.openBlock("std::int64_t Int(std::int64_t min_value, std::int64_t max_value) {");
    w.write("const auto pick = engine() % 10;");
    w.write("if (pick == 0 || min_value >= max_value) return min_value;");
    w.write("if (pick == 1) return max_value;");
    w.write("// The span is computed unsigned: wide ranges overflow int64 arithmetic.");
    w.write(
        "const std::uint64_t span = static_cast<std::uint64_t>(max_value) - "
            + "static_cast<std::uint64_t>(min_value);");
    w.write("return min_value + static_cast<std::int64_t>(engine() % (span + 1));");
    w.closeBlock("}");
    w.write("");
    w.openBlock("std::string Text(std::size_t min_len, std::size_t max_len) {");
    w.write(
        "static const char kAlphabet[] = "
            + "\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\";");
    w.write("const std::size_t n = Size(min_len, max_len);");
    w.write("std::string out;");
    w.write("out.reserve(n);");
    w.write("for (std::size_t i = 0; i < n; ++i) out.push_back(kAlphabet[engine() % 62]);");
    w.write("return out;");
    w.closeBlock("}");
    w.closeBlock("};");
    w.write("");
  }

  /** Emits Random<Shape> for every aggregate serde shape, in topological order. */
  void writeBuilders(CppWriter w) {
    for (Shape shape : SerdeGenerator.serdeShapes(context)) {
      writeBuilder(w, shape);
    }
  }

  private String builderName(Shape shape) {
    return "Random" + SerdeCodeGen.serdeFunctionSuffix(context, shape);
  }

  private void writeBuilder(CppWriter w, Shape shape) {
    String type = context.cppSymbols().toSymbol(shape).getName();
    // Both [[maybe_unused]]: builders are emitted for every serde shape, but
    // the round-trip suite only calls the unary operations' ones — a shape
    // reached solely through streaming operations goes unbuilt. And whether
    // rng itself is consumed depends on the shape: a memberless structure, or
    // one whose required members all pin constant values (bounded
    // @length/@range), never draws from it.
    w.openBlock("[[maybe_unused]] $L $L([[maybe_unused]] Rng& rng) {", type, builderName(shape));
    if (shape.isStructureShape()) {
      w.write("$L v{};", type);
      for (MemberShape member : shape.members()) {
        String field = "v." + context.cppSymbols().toMemberName(member);
        // @idempotencyToken members always get a value: the client would
        // auto-fill an unset one and break request equality.
        boolean alwaysSet =
            member.isRequired()
                || member.hasTrait(software.amazon.smithy.model.traits.IdempotencyTokenTrait.class);
        if (alwaysSet) {
          w.write("$L = $L;", field, expression(member));
        } else {
          w.write("if (rng.Coin()) $L = $L;", field, expression(member));
        }
      }
      w.write("return v;");
    } else if (shape.isUnionShape()) {
      UnionShape union = shape.asUnionShape().orElseThrow();
      var members = union.members().stream().toList();
      w.openBlock("switch (rng.engine() % $L) {", members.size());
      for (int i = 0; i < members.size(); ++i) {
        MemberShape member = members.get(i);
        String factory =
            software.amazon.smithy.utils.CaseUtils.toPascalCase(
                context.cppSymbols().toMemberName(member));
        if (i + 1 == members.size()) {
          w.write("default:").indent();
        } else {
          w.write("case $L:", i).indent();
        }
        w.write("return $L::From$L($L);", type, factory, expression(member));
        w.dedent();
      }
      w.closeBlock("}");
    } else if (shape.isListShape()) {
      ListShape list = shape.asListShape().orElseThrow();
      MemberShape element = list.getMember();
      boolean sparse = shape.hasTrait(SparseTrait.class);
      boolean unique = hasEffective(element, shape, UniqueItemsTrait.class);
      long[] size = containerSize(shape, unique);
      w.write("$L v{};", type);
      w.write("const std::size_t n = rng.Size($L, $L);", size[0], size[1]);
      w.openBlock("for (std::size_t i = 0; i < n; ++i) {");
      if (sparse) {
        w.write("if (!rng.fill_all && rng.engine() % 4 == 0) {");
        w.indent();
        w.write("v.push_back(std::nullopt);");
        w.write("continue;");
        w.dedent();
        w.write("}");
      }
      if (unique && target(element).isStringShape()) {
        // Distinct entries so @uniqueItems validation passes.
        w.write("v.push_back($L + std::to_string(i));", expression(element));
      } else {
        w.write("v.push_back($L);", expression(element));
      }
      w.closeBlock("}");
      w.write("return v;");
    } else if (shape.isMapShape()) {
      MapShape map = shape.asMapShape().orElseThrow();
      long[] size = containerSize(shape, false);
      w.write("$L v{};", type);
      w.write("const std::size_t n = rng.Size($L, $L);", size[0], size[1]);
      w.openBlock("for (std::size_t i = 0; i < n; ++i) {");
      // The index suffix keeps keys distinct; key constraints still hold
      // because the base stays within bounds and the suffix is one digit.
      w.write(
          "v.insert_or_assign($L + std::to_string(i), $L);",
          keyExpression(map.getKey()),
          expression(map.getValue()));
      w.closeBlock("}");
      w.write("return v;");
    } else {
      throw new CodegenException("cpp-codegen: no random builder for " + shape.getId());
    }
    w.closeBlock("}");
    w.write("");
  }

  private static <T extends Trait> boolean hasEffective(
      MemberShape member, Shape container, Class<T> t) {
    return member.hasTrait(t) || container.hasTrait(t);
  }

  /** min/max entry count for a container, honoring @length and keeping tests fast. */
  private long[] containerSize(Shape shape, boolean unique) {
    Optional<LengthTrait> length = shape.getTrait(LengthTrait.class);
    long min = Math.max(1, length.flatMap(LengthTrait::getMin).orElse(1L));
    long cap = unique ? Math.max(min, 2) : min + 2;
    long max = Math.min(length.flatMap(LengthTrait::getMax).orElse(cap), cap);
    return new long[] {min, Math.max(min, max)};
  }

  /** Map keys are plain strings; honor key-member constraints minus the digit suffix. */
  private String keyExpression(MemberShape key) {
    Optional<PatternTrait> pattern = constraint(key, PatternTrait.class);
    long min = constraint(key, LengthTrait.class).flatMap(LengthTrait::getMin).orElse(1L);
    if (pattern.isPresent()) {
      return "std::string(" + CppLiterals.stringLiteral(patternCandidate(pattern.get(), min)) + ")";
    }
    long max =
        Math.min(
            constraint(key, LengthTrait.class).flatMap(LengthTrait::getMax).orElse(min + 6) - 1,
            min + 6);
    return "rng.Text(" + Math.max(1, min) + ", " + Math.max(Math.max(1, min), max) + ")";
  }

  /** A fixed string satisfying the pattern (best effort, Java-checked candidates). */
  private static String patternCandidate(PatternTrait pattern, long minLength) {
    int n = (int) Math.max(minLength, 1);
    for (String fill : new String[] {"0", "a", "A"}) {
      String candidate = fill.repeat(n);
      try {
        if (java.util.regex.Pattern.compile(pattern.getValue()).matcher(candidate).find()) {
          return candidate;
        }
      } catch (java.util.regex.PatternSyntaxException e) {
        return candidate;
      }
    }
    return "0".repeat(n);
  }

  /** A C++ expression producing a random valid value for the member's target. */
  String expression(MemberShape member) {
    Shape target = target(member);
    if (target.getId().toString().equals("smithy.api#Unit")) {
      return "opal::Unit{}";
    }
    String type = context.cppSymbols().toSymbol(target).getName();
    return switch (target.getType()) {
      case BOOLEAN -> "(rng.engine() & 1U) != 0";
      case BYTE, SHORT, INTEGER, LONG -> {
        long[] bounds = intBounds(member, target);
        yield "static_cast<" + type + ">(rng.Int(" + bounds[0] + "LL, " + bounds[1] + "LL))";
      }
      case INT_ENUM -> {
        var values = target.asIntEnumShape().orElseThrow().getEnumValues().values();
        StringBuilder choices = new StringBuilder();
        for (Integer value : values) {
          if (choices.length() > 0) {
            choices.append(", ");
          }
          choices.append(value);
        }
        yield "static_cast<"
            + type
            + ">(std::array<int, "
            + values.size()
            + ">{"
            + choices
            + "}[rng.engine() % "
            + values.size()
            + "])";
      }
      case FLOAT, DOUBLE -> floatingExpression(member, target, type);
      case STRING -> stringExpression(member);
      case ENUM -> {
        var values = target.asEnumShape().orElseThrow().getEnumValues().values();
        StringBuilder choices = new StringBuilder();
        for (String value : values) {
          if (choices.length() > 0) {
            choices.append(", ");
          }
          choices.append(CppLiterals.stringLiteral(value));
        }
        yield type
            + "::FromString(std::array<const char*, "
            + values.size()
            + ">{"
            + choices
            + "}[rng.engine() % "
            + values.size()
            + "])";
      }
      case BLOB -> {
        long min =
            Math.max(
                1, constraint(member, LengthTrait.class).flatMap(LengthTrait::getMin).orElse(1L));
        long max =
            Math.min(
                constraint(member, LengthTrait.class).flatMap(LengthTrait::getMax).orElse(min + 8),
                min + 8);
        yield "opal::Blob::FromString(rng.Text(" + min + ", " + Math.max(min, max) + "))";
      }
      // Whole seconds survive every timestamp format (http-date has second
      // precision); the range stays http-date friendly (1970..2100).
      case TIMESTAMP -> "opal::Timestamp::FromEpochMilliseconds(rng.Int(0, 4102444799LL) * 1000)";
      case DOCUMENT ->
          "opal::Document(opal::DocumentMap{{\"key\", opal::Document(rng.Int(0, 1000))}})";
      case STRUCTURE, UNION, LIST, MAP -> builderName(target) + "(rng)";
      default -> throw new CodegenException("cpp-codegen: no random value for " + target.getId());
    };
  }

  private long[] intBounds(MemberShape member, Shape target) {
    long lo =
        switch (target.getType()) {
          case BYTE -> -128;
          case SHORT -> -32768;
          case INTEGER -> Integer.MIN_VALUE;
          default -> -4611686018427387904L;
        };
    long hi =
        switch (target.getType()) {
          case BYTE -> 127;
          case SHORT -> 32767;
          case INTEGER -> Integer.MAX_VALUE;
          default -> 4611686018427387903L;
        };
    Optional<RangeTrait> range = constraint(member, RangeTrait.class);
    if (range.isPresent()) {
      if (range.get().getMin().isPresent()) {
        lo = Math.max(lo, range.get().getMin().get().longValue());
      }
      if (range.get().getMax().isPresent()) {
        hi = Math.min(hi, range.get().getMax().get().longValue());
      }
    }
    return new long[] {lo, hi};
  }

  /** Dyadic eighths round-trip exactly through JSON/CBOR in both widths. */
  private String floatingExpression(MemberShape member, Shape target, String type) {
    Optional<RangeTrait> range = constraint(member, RangeTrait.class);
    if (range.isPresent()) {
      long lo = range.get().getMin().map(min -> (long) Math.ceil(min.doubleValue())).orElse(-1000L);
      long hi = range.get().getMax().map(max -> (long) Math.floor(max.doubleValue())).orElse(1000L);
      return "static_cast<" + type + ">(rng.Int(" + lo + "LL, " + hi + "LL))";
    }
    return "static_cast<"
        + type
        + ">(rng.Int(-8000000LL, 8000000LL)) / static_cast<"
        + type
        + ">(8)";
  }

  private String stringExpression(MemberShape member) {
    Optional<PatternTrait> pattern = constraint(member, PatternTrait.class);
    long min = constraint(member, LengthTrait.class).flatMap(LengthTrait::getMin).orElse(0L);
    if (pattern.isPresent()) {
      return "std::string(" + CppLiterals.stringLiteral(patternCandidate(pattern.get(), min)) + ")";
    }
    long lo = Math.max(1, min);
    long hi =
        Math.min(
            constraint(member, LengthTrait.class).flatMap(LengthTrait::getMax).orElse(lo + 8),
            lo + 8);
    return "rng.Text(" + lo + ", " + Math.max(lo, hi) + ")";
  }
}
