package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import software.amazon.smithy.codegen.core.Symbol;
import software.amazon.smithy.model.shapes.EnumShape;
import software.amazon.smithy.model.shapes.IntEnumShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.DocumentationTrait;
import software.amazon.smithy.model.traits.SensitiveTrait;
import software.amazon.smithy.utils.CaseUtils;

/** Emits C++ declarations for Smithy data shapes (the contract in docs/generated-types.md). */
final class TypeGenerators {

  private final CppContext context;
  private final CppWriter writer;

  TypeGenerators(CppContext context, CppWriter writer) {
    this.context = context;
    this.writer = writer;
  }

  private CppSymbolProvider symbols() {
    return context.cppSymbols();
  }

  private void writeDocs(Shape shape) {
    shape
        .getTrait(DocumentationTrait.class)
        .ifPresent(
            docs -> {
              for (String line : docs.getValue().split("\n", -1)) {
                writer.write("/// $L", line);
              }
            });
  }

  private String typeName(Shape shape) {
    return context.symbolProvider().toSymbol(shape).getName();
  }

  /** Enum constant for a member name: DRIP -> kDrip, dashEs-and_under -> kDashEsAndUnder. */
  static String enumConstant(String memberName) {
    return "k" + CaseUtils.toPascalCase(memberName.toLowerCase().replace('-', '_'));
  }

  /**
   * Fails generation when two members of the same shape fold to one C++ name, or a member collides
   * with a reserved synthetic name. Enum-constant and union-factory naming lower-case, strip
   * separators, or PascalCase the member name, so distinct Smithy members ({@code fooBar} and
   * {@code foo_bar}, or a member literally named {@code unknown}) can produce a duplicate
   * enumerator/method that no longer compiles. Catching it here names both members and the fix
   * instead of surfacing a C++ redefinition error in the generated output.
   */
  static void requireDistinctNames(
      String kind,
      Object shapeId,
      java.util.LinkedHashMap<String, String> foldedByMember,
      java.util.Set<String> reserved) {
    java.util.Map<String, String> owner = new java.util.HashMap<>();
    for (var entry : foldedByMember.entrySet()) {
      String member = entry.getKey();
      String folded = entry.getValue();
      if (reserved.contains(folded)) {
        throw new software.amazon.smithy.codegen.core.CodegenException(
            "cpp-codegen: "
                + kind
                + " "
                + shapeId
                + " member '"
                + member
                + "' maps to the reserved generated name '"
                + folded
                + "'; rename the member");
      }
      String prior = owner.putIfAbsent(folded, member);
      if (prior != null) {
        throw new software.amazon.smithy.codegen.core.CodegenException(
            "cpp-codegen: "
                + kind
                + " "
                + shapeId
                + " members '"
                + prior
                + "' and '"
                + member
                + "' both map to the generated name '"
                + folded
                + "'; rename one so their generated names differ");
      }
    }
  }

  /**
   * Fails generation when an operation's synthetic generated name (an error listing, a stream
   * alias) collides with a modeled shape's C++ name in the service namespace — the collision must
   * be an attributed diagnostic naming the fix, never silent misgeneration.
   */
  static void requireNoModelCollision(
      CppContext context,
      software.amazon.smithy.model.shapes.ServiceShape service,
      software.amazon.smithy.model.shapes.OperationShape operation,
      String syntheticName,
      String what) {
    String namespace = service.getId().getNamespace();
    java.util.stream.Stream<Shape> named =
        java.util.stream.Stream.of(
                context.model().getStructureShapes().stream(),
                context.model().getUnionShapes().stream(),
                context.model().getEnumShapes().stream(),
                context.model().getIntEnumShapes().stream())
            .flatMap(s -> s.map(Shape.class::cast));
    named
        .filter(shape -> shape.getId().getNamespace().equals(namespace))
        .filter(shape -> context.cppSymbols().toSymbol(shape).getName().equals(syntheticName))
        .findAny()
        .ifPresent(
            shape -> {
              throw new software.amazon.smithy.codegen.core.CodegenException(
                  "cpp-codegen: operation "
                      + operation.getId()
                      + " generates the "
                      + what
                      + " '"
                      + syntheticName
                      + "', which collides with shape "
                      + shape.getId()
                      + "; rename the shape or the operation");
            });
  }

  /**
   * Forward declarations for recursive member targets: on a cycle, the target's definition may come
   * later in types.h. Boxed members and std::vector elements only need the name declared; duplicate
   * declarations are harmless.
   */
  private void writeForwardDeclarations(StructureShape shape) {
    RecursionIndex recursion = symbols().recursion();
    if (!recursion.inCycle(shape.getId())) {
      return;
    }
    java.util.Set<String> declared = new java.util.TreeSet<>();
    for (MemberShape member : shape.members()) {
      collectCyclicStructNames(shape, member, declared);
    }
    for (String name : declared) {
      writer.write("struct $L;", name);
    }
    if (!declared.isEmpty()) {
      writer.write("");
    }
  }

  /** Struct names the member's type text references from the containing shape's cycle. */
  private void collectCyclicStructNames(
      StructureShape container, MemberShape member, java.util.Set<String> out) {
    RecursionIndex recursion = symbols().recursion();
    Shape target = context.model().expectShape(member.getTarget());
    if (target.isStructureShape()) {
      if (recursion.inCycle(target.getId()) && recursion.inCycle(container.getId())) {
        out.add(typeName(target));
      }
      return;
    }
    // Lists/maps inline their element type (std::vector<T> / std::map<K, T>),
    // so a cyclic element still appears in this struct's member text.
    if (target.isListShape()) {
      collectCyclicStructNames(container, target.asListShape().orElseThrow().getMember(), out);
    } else if (target.isMapShape()) {
      collectCyclicStructNames(container, target.asMapShape().orElseThrow().getValue(), out);
    }
  }

  void generateStructure(StructureShape shape) {
    String name = typeName(shape);
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (MemberShape member : shape.members()) {
      folded.put(member.getMemberName(), symbols().toMemberName(member));
    }
    // Structs carry the printing member functions, so those names are
    // reserved for data members now.
    requireDistinctNames(
        "structure", shape.getId(), folded, java.util.Set.of("AppendDebugTo", "DebugString"));
    writeForwardDeclarations(shape);
    writeDocs(shape);
    writer.openBlock("struct $L {", name);
    for (MemberShape member : shape.members()) {
      Symbol type = symbols().toMemberSymbol(member);
      writer.addIncludesFor(type);
      Shape target = context.model().expectShape(member.getTarget());
      boolean emptyAggregate = target.isListShape() || target.isMapShape();
      if (MemberDefaults.populated(context.model(), member) && !emptyAggregate) {
        // @default: plain member initialized to the default value.
        writer.write(
            "$L $L = $L;",
            type.getName(),
            symbols().toMemberName(member),
            MemberDefaults.literal(context, member));
      } else {
        writer.write("$L $L{};", type.getName(), symbols().toMemberName(member));
      }
    }
    if (!shape.members().isEmpty()) {
      writer.write("");
    }
    writeStructPrinting(shape, name);
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writeOrdering(writer, name, orderable(shape));
    writer.closeBlock("};");
    writer.write("");
    if (orderable(shape)) {
      writeStructHash(shape, name);
    }
  }

  /**
   * Designated-initializer-style debug rendering (issue #85): disengaged optionals are omitted,
   * members targeting a @sensitive shape (and the whole body of a @sensitive struct) print
   * [REDACTED] instead of leaking into logs. Member reads go through {@code this->} so a member
   * named {@code out} can't shadow the sink parameter.
   */
  private void writeStructPrinting(StructureShape shape, String name) {
    writer.addInclude("<ostream>").addInclude("<string>").addInclude("\"opal/core/print.h\"");
    writer.write("/// Debug rendering for logs and tests — for humans, never parse it.");
    writer.openBlock("void AppendDebugTo(std::string& out) const {");
    if (shape.hasTrait(SensitiveTrait.class)) {
      writer.write("out += $S;", name + "{[REDACTED]}");
    } else {
      writer.write("out += $S;", name + "{");
      if (!shape.members().isEmpty()) {
        writer.write("const char* sep = \"\";");
      }
      for (MemberShape member : shape.members()) {
        String memberName = symbols().toMemberName(member);
        boolean optional = symbols().toMemberSymbol(member).getName().startsWith("std::optional<");
        if (optional) {
          writer.openBlock("if (this->$L.has_value()) {", memberName);
        }
        writer.write("out += sep;");
        writer.write("sep = \", \";");
        writer.write("out += $S;", "." + memberName + " = ");
        if (context.model().expectShape(member.getTarget()).hasTrait(SensitiveTrait.class)) {
          writer.write("out += \"[REDACTED]\";");
        } else {
          writer.write("opal::DebugAppend(out, $Lthis->$L);", optional ? "*" : "", memberName);
        }
        if (optional) {
          writer.closeBlock("}");
        }
      }
      writer.write("out += '}';");
    }
    writer.closeBlock("}");
    writePrintingAdapters(writer, name);
    writer.write("");
  }

  /** DebugString() and operator<<: thin adapters over AppendDebugTo. */
  static void writePrintingAdapters(CppWriter writer, String name) {
    writer.write(
        "std::string DebugString() const { std::string out; AppendDebugTo(out); return out; }");
    writer.openBlock("friend std::ostream& operator<<(std::ostream& os, const $L& value) {", name);
    writer.write("return os << value.DebugString();");
    writer.closeBlock("}");
  }

  private boolean orderable(Shape shape) {
    return symbols().orderable(shape);
  }

  /**
   * Opens the file's post-namespace epilogue for a std::hash specialization (they must live at
   * global scope), writing the banner and includes on first use. Hashing follows ordering: callers
   * emit a specialization exactly for the types {@link #writeOrdering} gave an {@code operator<=>},
   * so ordered- and unordered-container keyability never diverge.
   */
  private static CppWriter hashEpilogue(CppWriter writer) {
    writer.addInclude("<cstddef>").addInclude("<functional>").addInclude("\"opal/core/hash.h\"");
    boolean first = !writer.hasEpilogue();
    CppWriter epilogue = writer.epilogue();
    if (first) {
      epilogue.write("// std::hash so generated types key std::unordered_map/std::unordered_set —");
      epilogue.write("// emitted exactly for the types that get operator<=> (issue #49). Hash");
      epilogue.write("// values are process-local: never persist or compare them across runs.");
      epilogue.write("");
    }
    return epilogue;
  }

  /** Member-wise hash for a struct, mirroring its defaulted member-wise operator==. */
  private void writeStructHash(StructureShape shape, String name) {
    CppWriter epilogue = hashEpilogue(writer);
    epilogue.write("template <>");
    epilogue.openBlock("struct std::hash<$L::$L> {", writer.cppNamespace(), name);
    if (shape.members().isEmpty()) {
      // No members to mix (and naming the parameter would trip -Wunused-parameter).
      epilogue.write(
          "std::size_t operator()(const $L::$L& /*value*/) const noexcept { return 0; }",
          writer.cppNamespace(),
          name);
    } else {
      epilogue.openBlock(
          "std::size_t operator()(const $L::$L& value) const noexcept {",
          writer.cppNamespace(),
          name);
      epilogue.write("std::size_t seed = 0;");
      for (MemberShape member : shape.members()) {
        epilogue.write(
            "seed = opal::HashCombine(seed, opal::HashValue(value.$L));",
            symbols().toMemberName(member));
      }
      epilogue.write("return seed;");
      epilogue.closeBlock("}");
    }
    epilogue.closeBlock("};");
    epilogue.write("");
  }

  /**
   * Emits the defaulted operator<=> when every member is orderable — otherwise a comment, so the
   * generated header never carries a defaulted-but-deleted operator (clang warns on those, and an
   * auto-returning deep <=> around a recursion cycle is a hard error; see generated-types.md's
   * ordering caveats).
   */
  static void writeOrdering(CppWriter writer, String name, boolean orderable) {
    if (orderable) {
      writer.addInclude("<compare>");
      writer.write("friend auto operator<=>(const $1L&, const $1L&) = default;", name);
    } else {
      writer.write("// Equality-only: a member type has no ordering (opal::Document or");
      writer.write("// recursion via opal::Boxed) — see generated-types.md.");
    }
  }

  void generateEnum(EnumShape shape) {
    String name = typeName(shape);
    Map<String, String> values = shape.getEnumValues();
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (String memberName : values.keySet()) {
      folded.put(memberName, enumConstant(memberName));
    }
    requireDistinctNames("enum", shape.getId(), folded, java.util.Set.of("kUnknown"));
    writer.addInclude("<string>").addInclude("<string_view>");

    writeDocs(shape);
    writer.openBlock("class $L {", name);
    writer.write("public:").indent();

    writer.openBlock("enum class Value {");
    for (String memberName : values.keySet()) {
      writer.write("$L,", enumConstant(memberName));
    }
    writer.write("kUnknown,");
    writer.closeBlock("};");
    writer.write("");

    writer.write("$L() = default;", name);
    writer.write("$1L(Value value) : value_(value) {}  // NOLINT(*-explicit-*)", name);
    writer.write("");

    writer.write("/// Unknown wire values are preserved and reported as Value::kUnknown.");
    writer.openBlock("static $L FromString(std::string_view text) {", name);
    for (Map.Entry<String, String> entry : values.entrySet()) {
      writer.write(
          "if (text == $S) return $L(Value::$L);",
          entry.getValue(),
          name,
          enumConstant(entry.getKey()));
    }
    writer.write("$L result;", name);
    writer.write("result.unknown_ = std::string(text);");
    writer.write("return result;");
    writer.closeBlock("}");
    writer.write("");

    writer.write("Value value() const { return value_; }");
    writer.write("");
    writer.write("/// Implicit so `switch (x)` works without .value(); the Value equality");
    writer.write("/// overload below keeps comparisons unambiguous despite the conversion.");
    writer.write("operator Value() const { return value_; }  // NOLINT(*-explicit-*)");
    writer.write("");
    writer.write("/// The wire text, including the original text of unknown values.");
    writer.openBlock("std::string_view ToString() const {");
    writer.openBlock("switch (value_) {");
    for (Map.Entry<String, String> entry : values.entrySet()) {
      writer.write("case Value::$L: return $S;", enumConstant(entry.getKey()), entry.getValue());
    }
    writer.write("case Value::kUnknown: return unknown_;");
    writer.closeBlock("}");
    writer.write("return unknown_;");
    writer.closeBlock("}");
    writer.write("");
    writer.addInclude("<ostream>");
    writer.write("/// Debug rendering for logs and tests — for humans, never parse it.");
    writer.openBlock("void AppendDebugTo(std::string& out) const {");
    writer.write("out += $S;", name + "(");
    if (shape.hasTrait(SensitiveTrait.class)) {
      writer.write("out += \"[REDACTED]\";");
    } else {
      writer.write("out += ToString();");
    }
    writer.write("out += ')';");
    writer.closeBlock("}");
    writePrintingAdapters(writer, name);
    writer.write("");
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writer.write("friend bool operator==(const $L& a, Value b) { return a.value_ == b; }", name);
    writeOrdering(writer, name, true);
    writer.write("friend struct std::hash<$L>;", name);
    writer.write("").dedent();

    writer.write("private:").indent();
    writer.write("Value value_ = Value::kUnknown;");
    writer.write("std::string unknown_;").dedent();
    writer.closeBlock("};");
    writer.write("");
    writeEnumHash(name);
  }

  /** Hash over the (value, unknown-text) pair — the fields the defaulted operator== compares. */
  private void writeEnumHash(String name) {
    CppWriter epilogue = hashEpilogue(writer);
    epilogue.write("template <>");
    epilogue.openBlock("struct std::hash<$L::$L> {", writer.cppNamespace(), name);
    epilogue.openBlock(
        "std::size_t operator()(const $L::$L& value) const noexcept {",
        writer.cppNamespace(),
        name);
    epilogue.write("return opal::HashCombine(static_cast<std::size_t>(value.value_),");
    epilogue.write("                           opal::HashValue(value.unknown_));");
    epilogue.closeBlock("}");
    epilogue.closeBlock("};");
    epilogue.write("");
  }

  void generateIntEnum(IntEnumShape shape) {
    String name = typeName(shape);
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (String memberName : shape.getEnumValues().keySet()) {
      folded.put(memberName, enumConstant(memberName));
    }
    requireDistinctNames("intEnum", shape.getId(), folded, java.util.Set.of());
    writer.addInclude("<cstdint>");
    writeDocs(shape);
    writer.openBlock("enum class $L : std::int32_t {", name);
    for (Map.Entry<String, Integer> entry : shape.getEnumValues().entrySet()) {
      writer.write("$L = $L,", enumConstant(entry.getKey()), entry.getValue());
    }
    writer.closeBlock("};");
    writer.write("");
  }

  void generateUnion(UnionShape shape) {
    String name = typeName(shape);
    List<MemberShape> members = List.copyOf(shape.members());
    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (MemberShape member : members) {
      folded.put(
          member.getMemberName(), "From" + CaseUtils.toPascalCase(symbols().toMemberName(member)));
    }
    requireDistinctNames("union", shape.getId(), folded, java.util.Set.of());

    List<TaggedMember> tagged = new java.util.ArrayList<>();
    for (MemberShape member : members) {
      Shape target = context.model().expectShape(member.getTarget());
      Symbol type = context.symbolProvider().toSymbol(target);
      writer.addIncludesFor(type);
      tagged.add(
          new TaggedMember(
              symbols().toMemberName(member),
              type.getName(),
              target.hasTrait(SensitiveTrait.class)));
    }
    writeDocs(shape);
    emitTaggedVariant(
        writer,
        name,
        tagged,
        /* withFactories= */ true,
        orderable(shape),
        shape.hasTrait(SensitiveTrait.class),
        "/// True until one of the From* factories has been used.",
        "/// Name of the engaged member, \"(empty)\" until a From* factory has run.",
        null);
  }

  /**
   * One alternative of a tagged-variant class: accessor stem + C++ type name, plus whether the
   * member's target shape is @sensitive (its debug rendering redacts).
   */
  record TaggedMember(String memberName, String typeName, boolean sensitive) {}

  /**
   * The tagged-variant class shape shared by generated unions and per-operation error listings:
   * is_x/as_x/as_x_or_null accessors over {@code std::variant<std::monostate, ...>}, empty(),
   * case_name(), visit(), equality, and the contextful wrong-case guard (ADR-0009). Callers add
   * member-type includes; {@code extraPublic} (nullable) emits caller-specific members right after
   * the default constructor — the error listings' FromError factory.
   */
  static void emitTaggedVariant(
      CppWriter writer,
      String name,
      List<TaggedMember> members,
      boolean withFactories,
      boolean withOrdering,
      boolean redactBody,
      String emptyDoc,
      String caseNameDoc,
      Runnable extraPublic) {
    writer.addInclude("<cstddef>").addInclude("<utility>").addInclude("<variant>");
    writer.addInclude("<ostream>").addInclude("<string>").addInclude("\"opal/core/print.h\"");
    writer.addInclude("\"opal/core/fatal.h\"");

    writer.openBlock("class $L {", name);
    writer.write("public:").indent();
    writer.write("$L() = default;", name);
    writer.write("");
    if (extraPublic != null) {
      extraPublic.run();
      writer.write("");
    }
    for (int i = 0; i < members.size(); ++i) {
      TaggedMember member = members.get(i);
      int index = i + 1; // index 0 is the unset monostate
      if (withFactories) {
        writer.openBlock(
            "static $L From$L($L value) {",
            name,
            CaseUtils.toPascalCase(member.memberName()),
            member.typeName());
        writer.write("$L result;", name);
        writer.write("result.value_.emplace<$L>(std::move(value));", index);
        writer.write("return result;");
        writer.closeBlock("}");
      }
      writer.write(
          "bool is_$L() const { return value_.index() == $L; }", member.memberName(), index);
      writer.openBlock("const $L& as_$L() const {", member.typeName(), member.memberName());
      writer.write("require_is($L, $S);", index, member.memberName());
      writer.write("return std::get<$L>(value_);", index);
      writer.closeBlock("}");
      writer.write("/// The engaged member, or nullptr when another member (or none) is set.");
      writer.write(
          "const $L* as_$L_or_null() const { return std::get_if<$L>(&value_); }",
          member.typeName(),
          member.memberName(),
          index);
      writer.write("");
    }
    writer.write(emptyDoc);
    writer.write("bool empty() const { return value_.index() == 0; }");
    writer.write("");
    String caseNames =
        java.util.stream.Stream.concat(
                java.util.stream.Stream.of("(empty)"),
                members.stream().map(TaggedMember::memberName))
            .map(CppLiterals::stringLiteral)
            .collect(java.util.stream.Collectors.joining(", "));
    writer.write(caseNameDoc);
    writer.openBlock("const char* case_name() const {");
    writer.write("static constexpr const char* kNames[] = {$L};", caseNames);
    writer.write("return kNames[value_.index()];");
    writer.closeBlock("}");
    writer.write("");
    writer.write("/// Applies `visitor` to the engaged member. The visitor must also accept");
    writer.write("/// std::monostate, which represents the empty state.");
    writer.write("template <typename Visitor>");
    writer.openBlock("decltype(auto) visit(Visitor&& visitor) const {");
    writer.write("return std::visit(std::forward<Visitor>(visitor), value_);");
    writer.closeBlock("}");
    writer.write("");
    writer.write("/// Debug rendering for logs and tests — for humans, never parse it.");
    writer.openBlock("void AppendDebugTo(std::string& out) const {");
    if (redactBody) {
      writer.write("out += $S;", name + "([REDACTED])");
    } else {
      writer.write("out += $S;", name + "(");
      writer.openBlock("switch (value_.index()) {");
      for (int i = 0; i < members.size(); ++i) {
        TaggedMember member = members.get(i);
        writer.write("case $L:", i + 1);
        writer.indent();
        writer.write("out += $S;", member.memberName() + " = ");
        if (member.sensitive()) {
          writer.write("out += \"[REDACTED]\";");
        } else {
          writer.write("opal::DebugAppend(out, std::get<$L>(value_));", i + 1);
        }
        writer.write("break;");
        writer.dedent();
      }
      writer.write("default:");
      writer.indent();
      writer.write("break;");
      writer.dedent();
      writer.closeBlock("}");
      writer.write("out += ')';");
    }
    writer.closeBlock("}");
    writePrintingAdapters(writer, name);
    writer.write("");
    writer.write("friend bool operator==(const $1L&, const $1L&) = default;", name);
    writeOrdering(writer, name, withOrdering);
    if (withOrdering) {
      writer.write("friend struct std::hash<$L>;", name);
    }
    writer.write("").dedent();

    writer.write("private:").indent();
    writer.openBlock("void require_is(std::size_t index, const char* requested) const {");
    writer.openBlock("if (value_.index() != index) {");
    writer.write("opal::internal::FatalWrongUnionAccess($S, requested, case_name());", name);
    writer.closeBlock("}");
    writer.closeBlock("}");
    writer.write("");
    String variant =
        members.stream()
            .map(TaggedMember::typeName)
            .collect(
                java.util.stream.Collectors.joining(
                    ", ", "std::variant<std::monostate, ", "> value_;"));
    writer.write("$L", variant).dedent();
    writer.closeBlock("};");
    writer.write("");
    if (withOrdering) {
      writeTaggedVariantHash(writer, name);
    }
  }

  /** Hash over (engaged index, engaged member) — what the defaulted operator== compares. */
  private static void writeTaggedVariantHash(CppWriter writer, String name) {
    CppWriter epilogue = hashEpilogue(writer);
    epilogue.write("template <>");
    epilogue.openBlock("struct std::hash<$L::$L> {", writer.cppNamespace(), name);
    epilogue.openBlock(
        "std::size_t operator()(const $L::$L& value) const noexcept {",
        writer.cppNamespace(),
        name);
    epilogue.write("const std::size_t member =");
    epilogue.write(
        "    std::visit([](const auto& v) { return opal::HashValue(v); }, value.value_);");
    epilogue.write("return opal::HashCombine(value.value_.index(), member);");
    epilogue.closeBlock("}");
    epilogue.closeBlock("};");
    epilogue.write("");
  }
}
