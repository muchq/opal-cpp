package io.smithycpp.codegen;

import alloy.DiscriminatedUnionTrait;
import alloy.JsonUnknownTrait;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import software.amazon.smithy.codegen.core.Symbol;
import software.amazon.smithy.codegen.core.TopologicalIndex;
import software.amazon.smithy.model.neighbor.Walker;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.SparseTrait;

/**
 * Emits serde.h / src/serde.cc: Document-pivot serializers and deserializers for every aggregate
 * shape in the service closure (both directions — clients, servers, and tests all reuse them).
 */
final class SerdeGenerator {

  private final CppContext context;
  private final SerdeCodeGen serde;

  SerdeGenerator(CppContext context, boolean useJsonName) {
    this.context = context;
    this.serde = new SerdeCodeGen(context, useJsonName);
  }

  List<Shape> serdeShapes() {
    return serdeShapes(context);
  }

  /**
   * Aggregate shapes in the closure, topologically ordered, excluding smithy.api#Unit. Static so
   * callers that only need the ordering (RandomValueGenerator, ValidationGenerator) don't have to
   * construct a generator — and pick a wire-name policy — to get it.
   */
  static List<Shape> serdeShapes(CppContext context) {
    Set<Shape> closure =
        new Walker(context.model())
            .walkShapes(context.model().expectShape(context.settings().service()));
    List<Shape> ordered = new ArrayList<>();
    for (Shape shape : TopologicalIndex.of(context.model()).getOrderedShapes()) {
      if (serdeShape(closure, shape)) {
        ordered.add(shape);
      }
    }
    // Shapes on recursion cycles are not topologically orderable; they come
    // last, in stable id order. Serde functions are all declared in serde.h
    // (mutual recursion is fine) and types.h forward-declares cycle members.
    List<Shape> recursive = new ArrayList<>();
    for (Shape shape : TopologicalIndex.of(context.model()).getRecursiveShapes()) {
      if (serdeShape(closure, shape)) {
        recursive.add(shape);
      }
    }
    recursive.sort(java.util.Comparator.comparing(Shape::getId));
    ordered.addAll(recursive);
    return ordered;
  }

  private static boolean serdeShape(Set<Shape> closure, Shape shape) {
    return closure.contains(shape) && hasSerdeFunctions(shape);
  }

  /**
   * Whether serde.cc emits Serialize/Deserialize functions for this shape. Enums convert through
   * FromString/ToString instead, and smithy.api#Unit never crosses the wire.
   */
  static boolean hasSerdeFunctions(Shape shape) {
    if (shape.getId().toString().equals("smithy.api#Unit")) {
      return false;
    }
    return shape.isStructureShape()
        || shape.isUnionShape()
        || shape.isListShape()
        || shape.isMapShape();
  }

  void run() {
    List<Shape> shapes = serdeShapes();
    if (shapes.isEmpty()) {
      return;
    }
    CppSettings settings = context.settings();
    context
        .writerDelegator()
        .useFileWriter(settings.serdeHeaderFile(), w -> writeHeader(w, shapes));
    context.writerDelegator().useFileWriter("src/serde.cc", w -> writeSource(w, shapes));
  }

  private String valueType(Shape shape) {
    return context.cppSymbols().toSymbol(shape).getName();
  }

  private void writeHeader(CppWriter w, List<Shape> shapes) {
    w.addInclude("\"opal/core/document.h\"");
    w.addInclude("\"opal/core/outcome.h\"");
    w.addInclude("\"" + context.settings().includePrefix() + "/types.h\"");
    w.write("// Document-pivot serde for every aggregate shape in the model closure.");
    w.write("// Serializers never fail; deserializers return opal::Error on wire");
    w.write("// mismatches and enforce @required members.");
    w.write("");
    for (Shape shape : shapes) {
      String suffix = SerdeCodeGen.serdeFunctionSuffix(context, shape);
      String type = valueType(shape);
      w.addIncludesFor(context.cppSymbols().toSymbol(shape));
      w.write("opal::Document Serialize$L(const $L& value);", suffix, type);
      w.write("opal::Outcome<$L> Deserialize$L(const opal::Document& doc);", type, suffix);
      w.write("");
    }
  }

  private void writeSource(CppWriter w, List<Shape> shapes) {
    w.addInclude("\"" + context.settings().includePrefix() + "/serde.h\"");
    w.addInclude("\"opal/core/document_serde.h\"");
    w.addInclude("<string>");
    w.addInclude("<utility>");
    for (Shape shape : shapes) {
      if (shape.isStructureShape()) {
        writeStructure(w, shape.asStructureShape().orElseThrow());
      } else if (shape.isUnionShape()) {
        writeUnion(w, shape.asUnionShape().orElseThrow());
      } else if (shape.isListShape()) {
        writeList(w, shape.asListShape().orElseThrow());
      } else {
        writeMap(w, shape.asMapShape().orElseThrow());
      }
    }
  }

  private void writeStructure(CppWriter w, StructureShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(context, shape);
    String type = valueType(shape);

    // A memberless structure serializes to the empty map without reading the
    // value; the parameter stays unnamed (Core Guidelines F.9) so the function
    // is clean under -Wextra's -Wunused-parameter.
    String valueParam = shape.members().isEmpty() ? "/*value*/" : "value";
    w.openBlock("opal::Document Serialize$L(const $L& $L) {", suffix, type, valueParam);
    w.write("opal::DocumentMap map;");
    for (MemberShape member : shape.members()) {
      serde.writeMemberSerialize(w, member, "value", "map");
    }
    w.write("return opal::Document(std::move(map));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("opal::Outcome<$L> Deserialize$L(const opal::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return opal::Error::Serialization($S);",
        type + ": expected a map on the wire");
    w.write("$L out;", type);
    for (MemberShape member : shape.members()) {
      serde.writeMemberRead(
          w,
          member,
          "doc.",
          "out.",
          type,
          /* fillDefaults= */ true,
          (w2, m, deserializeMember) -> {
            // Serde is strict on both wire ends: absence fails the parse with
            // the type-qualified message (by wire name, unlike the clients'
            // exchange-level message).
            w2.openBlock("if ($L) {", SerdeCodeGen.MEMBER_ABSENT);
            w2.write(
                "return opal::Error::Serialization($S);",
                type + ": missing required member: " + serde.wireName(m));
            w2.closeBlock("}");
            deserializeMember.run();
          });
    }
    w.write("return out;");
    w.closeBlock("}");
    w.write("");
  }

  private void writeUnion(CppWriter w, UnionShape shape) {
    if (shape.hasTrait(DiscriminatedUnionTrait.class)) {
      writeDiscriminatedUnion(w, shape);
      return;
    }
    String suffix = SerdeCodeGen.serdeFunctionSuffix(context, shape);
    String type = valueType(shape);
    // alloy's @jsonUnknown member (open unions) carries the entire wire object
    // for unrecognized tags; it has no tag key of its own.
    Optional<MemberShape> unknown = jsonUnknownMember(shape);

    w.openBlock("opal::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("opal::DocumentMap map;");
    for (MemberShape member : shape.members()) {
      String name = context.cppSymbols().toMemberName(member);
      if (isJsonUnknown(member)) {
        w.write("if (value.is_$L()) return value.as_$L();", name, name);
        continue;
      }
      w.openBlock("if (value.is_$L()) {", name);
      w.write(
          "map.emplace($S, $L);",
          serde.wireName(member),
          serde.serializeExpression(member, "value.as_" + name + "()"));
      w.closeBlock("}");
    }
    w.write("return opal::Document(std::move(map));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("opal::Outcome<$L> Deserialize$L(const opal::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return opal::Error::Serialization($S);",
        type + ": expected a map on the wire");
    // Unions are exactly one member on the wire; extra known or unknown
    // members are malformed (the malformed-request suite pins this), but a
    // "__type" hint is ignored (clients must tolerate it in responses).
    // Open unions route anything else to the @jsonUnknown member instead.
    if (unknown.isPresent()) {
      w.openBlock("if (doc.as_map().size() - (doc.Find(\"__type\") != nullptr ? 1 : 0) == 1) {");
    } else {
      w.write(
          "if (doc.as_map().size() - (doc.Find(\"__type\") != nullptr ? 1 : 0) != 1) "
              + "return opal::Error::Serialization($S);",
          type + ": expected exactly one union member");
    }
    for (MemberShape member : shape.members()) {
      if (isJsonUnknown(member)) {
        continue;
      }
      String wireName = serde.wireName(member);
      Symbol targetType =
          context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));
      w.openBlock(
          "if (const opal::Document* member = doc.Find($S);"
              + " member != nullptr && !member->is_null()) {",
          wireName);
      w.write("$L parsed_member{};", targetType.getName());
      serde.writeDeserializeInto(w, member, "member", "parsed_member", type + "." + wireName);
      w.write(
          "return $L::From$L(std::move(parsed_member));",
          type,
          pascal(context.cppSymbols().toMemberName(member)));
      w.closeBlock("}");
    }
    if (unknown.isPresent()) {
      w.closeBlock("}");
      w.write(
          "return $L::From$L(doc);",
          type,
          pascal(context.cppSymbols().toMemberName(unknown.get())));
    } else {
      w.write("return opal::Error::Serialization($S);", type + ": unknown or missing union member");
    }
    w.closeBlock("}");
    w.write("");
  }

  /**
   * alloy @discriminated unions: the wire form is the engaged member's own object with the
   * discriminator field spliced in ({"key": "smol", ...member fields...}), not a single-key tagged
   * wrapper. A @jsonUnknown member (open union) keeps the whole object — discriminator included —
   * when the discriminator value matches no known member.
   */
  private void writeDiscriminatedUnion(CppWriter w, UnionShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(context, shape);
    String type = valueType(shape);
    String discriminator = shape.expectTrait(DiscriminatedUnionTrait.class).getValue();
    Optional<MemberShape> unknown = jsonUnknownMember(shape);

    w.openBlock("opal::Document Serialize$L(const $L& value) {", suffix, type);
    for (MemberShape member : shape.members()) {
      String name = context.cppSymbols().toMemberName(member);
      if (isJsonUnknown(member)) {
        w.write("if (value.is_$L()) return value.as_$L();", name, name);
        continue;
      }
      w.openBlock("if (value.is_$L()) {", name);
      w.write(
          "opal::Document member_doc = $L;",
          serde.serializeExpression(member, "value.as_" + name + "()"));
      w.write(
          "member_doc.as_map().insert_or_assign($S, opal::Document(std::string($S)));",
          discriminator,
          serde.wireName(member));
      w.write("return member_doc;");
      w.closeBlock("}");
    }
    w.write("return opal::Document(opal::DocumentMap{});");
    w.closeBlock("}");
    w.write("");

    w.openBlock("opal::Outcome<$L> Deserialize$L(const opal::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return opal::Error::Serialization($S);",
        type + ": expected a map on the wire");
    w.openBlock(
        "if (const opal::Document* discriminator = doc.Find($S);"
            + " discriminator != nullptr && discriminator->is_string()) {",
        discriminator);
    for (MemberShape member : shape.members()) {
      if (isJsonUnknown(member)) {
        continue;
      }
      Symbol targetType =
          context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));
      w.openBlock("if (discriminator->as_string() == $S) {", serde.wireName(member));
      w.write("const opal::Document* member = &doc;");
      w.write("$L parsed_member{};", targetType.getName());
      serde.writeDeserializeInto(
          w, member, "member", "parsed_member", type + "." + serde.wireName(member));
      w.write(
          "return $L::From$L(std::move(parsed_member));",
          type,
          pascal(context.cppSymbols().toMemberName(member)));
      w.closeBlock("}");
    }
    w.closeBlock("}");
    if (unknown.isPresent()) {
      w.write(
          "return $L::From$L(doc);",
          type,
          pascal(context.cppSymbols().toMemberName(unknown.get())));
    } else {
      w.write("return opal::Error::Serialization($S);", type + ": unknown or missing union member");
    }
    w.closeBlock("}");
    w.write("");
  }

  private static Optional<MemberShape> jsonUnknownMember(UnionShape shape) {
    return shape.members().stream().filter(SerdeGenerator::isJsonUnknown).findFirst();
  }

  private static boolean isJsonUnknown(MemberShape member) {
    return member.hasTrait(JsonUnknownTrait.class);
  }

  private void writeList(CppWriter w, ListShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(context, shape);
    String type = valueType(shape);
    MemberShape member = shape.getMember();
    boolean sparse = shape.hasTrait(SparseTrait.class);
    Symbol element = context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));

    w.openBlock("opal::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("opal::DocumentList list;");
    w.write("list.reserve(value.size());");
    w.openBlock("for (const auto& item : value) {");
    if (sparse) {
      w.openBlock("if (!item.has_value()) {");
      w.write("list.emplace_back(nullptr);");
      w.write("continue;");
      w.closeBlock("}");
      w.write("list.push_back($L);", serde.serializeExpression(member, "(*item)"));
    } else {
      w.write("list.push_back($L);", serde.serializeExpression(member, "item"));
    }
    w.closeBlock("}");
    w.write("return opal::Document(std::move(list));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("opal::Outcome<$L> Deserialize$L(const opal::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_list()) return opal::Error::Serialization($S);",
        type + ": expected a list on the wire");
    w.write("$L out;", type);
    w.write("out.reserve(doc.as_list().size());");
    w.openBlock("for (const opal::Document& item_doc : doc.as_list()) {");
    w.write("const opal::Document* item = &item_doc;");
    if (sparse) {
      w.openBlock("if (item->is_null()) {");
      w.write("out.emplace_back(std::nullopt);");
      w.write("continue;");
      w.closeBlock("}");
    } else {
      w.write(
          "if (item->is_null()) return opal::Error::Serialization($S);",
          type + ": null element in a dense list");
    }
    w.write("$L parsed_item{};", element.getName());
    serde.writeDeserializeInto(w, member, "item", "parsed_item", type + "[]");
    w.write("out.push_back(std::move(parsed_item));");
    w.closeBlock("}");
    w.write("return out;");
    w.closeBlock("}");
    w.write("");
  }

  private void writeMap(CppWriter w, MapShape shape) {
    String suffix = SerdeCodeGen.serdeFunctionSuffix(context, shape);
    String type = valueType(shape);
    MemberShape member = shape.getValue();
    boolean sparse = shape.hasTrait(SparseTrait.class);
    Symbol element = context.cppSymbols().toSymbol(context.model().expectShape(member.getTarget()));

    w.openBlock("opal::Document Serialize$L(const $L& value) {", suffix, type);
    w.write("opal::DocumentMap map;");
    w.openBlock("for (const auto& [key, item] : value) {");
    if (sparse) {
      w.openBlock("if (!item.has_value()) {");
      w.write("map.emplace(key, opal::Document(nullptr));");
      w.write("continue;");
      w.closeBlock("}");
      w.write("map.emplace(key, $L);", serde.serializeExpression(member, "(*item)"));
    } else {
      w.write("map.emplace(key, $L);", serde.serializeExpression(member, "item"));
    }
    w.closeBlock("}");
    w.write("return opal::Document(std::move(map));");
    w.closeBlock("}");
    w.write("");

    w.openBlock("opal::Outcome<$L> Deserialize$L(const opal::Document& doc) {", type, suffix);
    w.write(
        "if (!doc.is_map()) return opal::Error::Serialization($S);",
        type + ": expected a map on the wire");
    w.write("$L out;", type);
    w.openBlock("for (const auto& [key, item_doc] : doc.as_map()) {");
    w.write("const opal::Document* item = &item_doc;");
    if (sparse) {
      w.openBlock("if (item->is_null()) {");
      w.write("out.emplace(key, std::nullopt);");
      w.write("continue;");
      w.closeBlock("}");
    } else {
      w.write("// Tolerant read: null values in dense maps are skipped, not errors.");
      w.write("if (item->is_null()) continue;");
    }
    w.write("$L parsed_item{};", element.getName());
    serde.writeDeserializeInto(w, member, "item", "parsed_item", type + "{}");
    w.write("out.emplace(key, std::move(parsed_item));");
    w.closeBlock("}");
    w.write("return out;");
    w.closeBlock("}");
    w.write("");
  }

  static String pascal(String name) {
    return software.amazon.smithy.utils.CaseUtils.toPascalCase(name);
  }
}
