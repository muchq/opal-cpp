package io.smithycpp.codegen;

import java.util.Set;
import java.util.TreeSet;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.codegen.core.Symbol;
import software.amazon.smithy.codegen.core.SymbolProvider;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.shapes.BigDecimalShape;
import software.amazon.smithy.model.shapes.BigIntegerShape;
import software.amazon.smithy.model.shapes.BlobShape;
import software.amazon.smithy.model.shapes.BooleanShape;
import software.amazon.smithy.model.shapes.ByteShape;
import software.amazon.smithy.model.shapes.DocumentShape;
import software.amazon.smithy.model.shapes.DoubleShape;
import software.amazon.smithy.model.shapes.EnumShape;
import software.amazon.smithy.model.shapes.FloatShape;
import software.amazon.smithy.model.shapes.IntEnumShape;
import software.amazon.smithy.model.shapes.IntegerShape;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.LongShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ResourceShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeVisitor;
import software.amazon.smithy.model.shapes.ShortShape;
import software.amazon.smithy.model.shapes.StringShape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.TimestampShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.SparseTrait;

/**
 * Maps Smithy shapes to C++ types (the naming contract in docs/generated-types.md).
 *
 * <p>Each returned {@link Symbol}'s name is the full C++ type text; the headers it needs are
 * carried in the {@code headers} property (angle form {@code <vector>} or quote form {@code
 * "smithy/core/blob.h"}).
 */
final class CppSymbolProvider implements SymbolProvider {

  static final String HEADERS_PROPERTY = "headers";

  private final Model model;
  private final CppSettings settings;
  private final Visitor visitor;
  private final RecursionIndex recursion;

  CppSymbolProvider(Model model, CppSettings settings) {
    this.model = model;
    this.settings = settings;
    this.visitor = new Visitor();
    this.recursion = new RecursionIndex(model);
  }

  /** The model's recursion cycles (boxed members, forward declarations). */
  RecursionIndex recursion() {
    return recursion;
  }

  private final java.util.Map<software.amazon.smithy.model.shapes.ShapeId, Boolean> orderable =
      new java.util.HashMap<>();

  /**
   * Whether the shape's C++ type is three-way-comparable, i.e. whether the generators emit a
   * defaulted operator<=> for it (docs/generated-types.md's ordering caveats): nothing in its
   * member closure is a document (opal::Document has no ordering) or on a recursion cycle
   * (opal::Boxed is deliberately equality-only — an auto-returning deep <=> is a hard error to
   * deduce around a cycle on clang). Memoized for the run.
   */
  boolean orderable(Shape shape) {
    return orderable.computeIfAbsent(
        shape.getId(),
        id ->
            new software.amazon.smithy.model.neighbor.Walker(model)
                .walkShapes(shape).stream()
                    .noneMatch(s -> s.isDocumentShape() || recursion.inCycle(s.getId())));
  }

  @Override
  public Symbol toSymbol(Shape shape) {
    return shape.accept(visitor);
  }

  @Override
  public String toMemberName(MemberShape member) {
    return CppReservedWords.escape(member.getMemberName());
  }

  /**
   * Full member type text: the target type, wrapped in opal::Boxed for recursive structure members
   * and in std::optional unless @required or populated by @default.
   */
  Symbol toMemberSymbol(MemberShape member) {
    Symbol target = toSymbol(model.expectShape(member.getTarget()));
    String name = target.getName();
    Set<String> headers = new TreeSet<>(headersOf(target));
    boolean plain = MemberDefaults.plain(model, member);
    if (recursion.isBoxed(member)) {
      name = "opal::Boxed<" + name + ">";
      headers.add("\"smithy/core/boxed.h\"");
    } else if (plain) {
      return target;
    }
    if (plain) {
      return builder(name, headers).build();
    }
    headers.add("<optional>");
    return builder("std::optional<" + name + ">", headers).build();
  }

  @SuppressWarnings("unchecked")
  static Set<String> headersOf(Symbol symbol) {
    return symbol.getProperty(HEADERS_PROPERTY).map(value -> (Set<String>) value).orElse(Set.of());
  }

  private Symbol.Builder builder(String name, Set<String> headers) {
    return Symbol.builder().name(name).putProperty(HEADERS_PROPERTY, headers);
  }

  private Symbol declared(Shape shape) {
    return builder(declaredName(shape), Set.of("\"" + settings.includePrefix() + "/types.h\""))
        .definitionFile(settings.typesHeaderFile())
        .build();
  }

  /**
   * The C++ identifier a shape declares (type name, serde-function suffix). Generated modules
   * flatten every Smithy namespace in the service closure into one C++ namespace, so shapes from
   * outside the service's own namespace get its last namespace segment appended when their plain
   * name collides (aws.example#Greeting + aws.shared#Greeting -> Greeting + GreetingShared).
   */
  String declaredName(Shape shape) {
    if (declaredNames == null) {
      declaredNames = computeDeclaredNames();
    }
    return declaredNames.getOrDefault(shape.getId(), escapeTypeName(shape.getId().getName()));
  }

  /**
   * Keyword escaping plus the two file-level identifiers generated sources claim (issue #71): a
   * type named {@code helpers} or {@code types} would be ambiguous against the helpers namespace /
   * the types alias, so it gets the same trailing-underscore treatment as a keyword. Member names
   * stay on plain {@link CppReservedWords} — member access never collides with a namespace.
   */
  private static String escapeTypeName(String name) {
    String escaped = CppReservedWords.escape(name);
    return escaped.equals("helpers") || escaped.equals("types") ? escaped + "_" : escaped;
  }

  /**
   * The spelling generated .cc code uses to reference a shape's C++ type where a file-local helper
   * could otherwise shadow it (issue #71): declared model types go through the file-level {@code
   * types} namespace alias ({@link CppWriter}); builtin mappings (std::string, float, opal::Blob,
   * ...) have no model-controlled name and stay bare.
   */
  String typeRef(Shape shape) {
    String name = toSymbol(shape).getName();
    // declaresType minus lists/maps: those spell std::vector<...>/std::map<...>
    // inline, so there is no declared identifier to route through the alias.
    boolean declared = declaresType(shape) && !shape.isListShape() && !shape.isMapShape();
    return declared ? "types::" + name : name;
  }

  /**
   * Whether the shape declares a C++ type name in the generated module — the filter behind {@link
   * #declaredName}. smithy.api#Unit maps to the runtime's opal::Unit and declares nothing.
   */
  static boolean declaresType(Shape shape) {
    return (shape.isStructureShape()
            || shape.isUnionShape()
            || shape.isEnumShape()
            || shape.isIntEnumShape()
            || shape.isListShape()
            || shape.isMapShape())
        && !shape.getId().toString().equals("smithy.api#Unit");
  }

  private java.util.Map<software.amazon.smithy.model.shapes.ShapeId, String> declaredNames;

  private java.util.Map<software.amazon.smithy.model.shapes.ShapeId, String>
      computeDeclaredNames() {
    java.util.Map<software.amazon.smithy.model.shapes.ShapeId, String> names =
        new java.util.HashMap<>();
    if (model.getShape(settings.service()).isEmpty()) {
      return names;
    }
    java.util.Map<String, java.util.List<Shape>> byName = new java.util.TreeMap<>();
    for (Shape shape :
        new software.amazon.smithy.model.neighbor.Walker(model)
            .walkShapes(model.expectShape(settings.service()))) {
      if (!declaresType(shape)) {
        continue;
      }
      byName
          .computeIfAbsent(
              escapeTypeName(shape.getId().getName()), key -> new java.util.ArrayList<>())
          .add(shape);
    }
    String homeNamespace = settings.service().getNamespace();
    for (var entry : byName.entrySet()) {
      if (entry.getValue().size() < 2) {
        continue;
      }
      for (Shape shape : entry.getValue()) {
        String namespace = shape.getId().getNamespace();
        if (namespace.equals(homeNamespace)) {
          continue; // The service's own namespace keeps the plain name.
        }
        String segment = namespace.substring(namespace.lastIndexOf('.') + 1);
        names.put(
            shape.getId(),
            entry.getKey() + software.amazon.smithy.utils.CaseUtils.toPascalCase(segment));
      }
    }
    java.util.Set<String> seen = new java.util.HashSet<>();
    for (var entry : byName.entrySet()) {
      for (Shape shape : entry.getValue()) {
        String name = names.getOrDefault(shape.getId(), entry.getKey());
        if (!seen.add(name)) {
          throw new CodegenException(
              "cpp-codegen: cannot disambiguate C++ type name '"
                  + name
                  + "' ("
                  + shape.getId()
                  + " still collides after namespace suffixing)");
        }
      }
    }
    return names;
  }

  private final class Visitor extends ShapeVisitor.Default<Symbol> {

    @Override
    protected Symbol getDefault(Shape shape) {
      throw new CodegenException(
          "cpp-codegen: shape type not supported yet: "
              + shape.getType()
              + " ("
              + shape.getId()
              + ")");
    }

    @Override
    public Symbol blobShape(BlobShape shape) {
      return builder("opal::Blob", Set.of("\"smithy/core/blob.h\"")).build();
    }

    @Override
    public Symbol booleanShape(BooleanShape shape) {
      return builder("bool", Set.of()).build();
    }

    @Override
    public Symbol byteShape(ByteShape shape) {
      return builder("std::int8_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol shortShape(ShortShape shape) {
      return builder("std::int16_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol integerShape(IntegerShape shape) {
      return builder("std::int32_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol longShape(LongShape shape) {
      return builder("std::int64_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol floatShape(FloatShape shape) {
      return builder("float", Set.of()).build();
    }

    @Override
    public Symbol doubleShape(DoubleShape shape) {
      return builder("double", Set.of()).build();
    }

    @Override
    public Symbol bigIntegerShape(BigIntegerShape shape) {
      throw new CodegenException("cpp-codegen: bigInteger is not supported yet: " + shape.getId());
    }

    @Override
    public Symbol bigDecimalShape(BigDecimalShape shape) {
      throw new CodegenException("cpp-codegen: bigDecimal is not supported yet: " + shape.getId());
    }

    @Override
    public Symbol stringShape(StringShape shape) {
      return builder("std::string", Set.of("<string>")).build();
    }

    @Override
    public Symbol enumShape(EnumShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol intEnumShape(IntEnumShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol timestampShape(TimestampShape shape) {
      return builder("opal::Timestamp", Set.of("\"smithy/core/timestamp.h\"")).build();
    }

    @Override
    public Symbol documentShape(DocumentShape shape) {
      return builder("opal::Document", Set.of("\"smithy/core/document.h\"")).build();
    }

    @Override
    public Symbol listShape(ListShape shape) {
      Symbol inner = elementSymbol(shape.getMember(), shape.hasTrait(SparseTrait.class));
      Set<String> headers = new TreeSet<>(headersOf(inner));
      headers.add("<vector>");
      return builder("std::vector<" + inner.getName() + ">", headers).build();
    }

    @Override
    public Symbol mapShape(MapShape shape) {
      Symbol inner = elementSymbol(shape.getValue(), shape.hasTrait(SparseTrait.class));
      Set<String> headers = new TreeSet<>(headersOf(inner));
      headers.add("<map>");
      headers.add("<string>");
      return builder("std::map<std::string, " + inner.getName() + ">", headers).build();
    }

    @Override
    public Symbol structureShape(StructureShape shape) {
      if (shape.getId().toString().equals("smithy.api#Unit")) {
        return builder("opal::Unit", Set.of("\"smithy/core/outcome.h\"")).build();
      }
      return declared(shape);
    }

    @Override
    public Symbol unionShape(UnionShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol serviceShape(ServiceShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol operationShape(OperationShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol resourceShape(ResourceShape shape) {
      return declared(shape);
    }

    private Symbol elementSymbol(MemberShape member, boolean sparse) {
      Symbol target = toSymbol(model.expectShape(member.getTarget()));
      if (!sparse) {
        return target;
      }
      Set<String> headers = new TreeSet<>(headersOf(target));
      headers.add("<optional>");
      return builder("std::optional<" + target.getName() + ">", headers).build();
    }
  }
}
