package io.smithycpp.codegen;

import java.util.Base64;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.traits.ClientOptionalTrait;
import software.amazon.smithy.model.traits.DefaultTrait;
import software.amazon.smithy.model.traits.InputTrait;

/**
 * @default value population (smithy.api#default). Members with a non-null default and no
 *     {@code @clientOptional} are "populated": plain (non-optional) C++ members initialized to the
 *     default, always serialized, and left at the default when missing from the wire — except on
 *     {@code @input} structures, whose members stay client-optional per the spec (clients skip
 *     unset members; servers fill the default while parsing, via {@link #fillOnParse}).
 */
final class MemberDefaults {

  private MemberDefaults() {}

  /** Plain C++ member with a default initializer (always serialized). */
  static boolean populated(Model model, MemberShape member) {
    return defaulted(model, member)
        && (member.isRequired()
            || !model.expectShape(member.getContainer()).hasTrait(InputTrait.class));
  }

  /** Optional C++ member the server assigns the default to when absent from a parsed input. */
  static boolean fillOnParse(Model model, MemberShape member) {
    return defaulted(model, member)
        && !member.isRequired()
        && model.expectShape(member.getContainer()).hasTrait(InputTrait.class);
  }

  /**
   * @required member with a @default (the evolution pattern): absence on the wire keeps the default
   *     instead of failing deserialization.
   */
  static boolean lenientRequired(Model model, MemberShape member) {
    return member.isRequired() && defaulted(model, member);
  }

  /** Whether the member is a plain (non-std::optional) C++ member. */
  static boolean plain(Model model, MemberShape member) {
    return member.isRequired() || populated(model, member);
  }

  private static boolean defaulted(Model model, MemberShape member) {
    if (member.hasTrait(ClientOptionalTrait.class)) {
      return false;
    }
    DefaultTrait trait = member.getTrait(DefaultTrait.class).orElse(null);
    if (trait == null || trait.toNode().isNullNode()) {
      return false;
    }
    return supported(model.expectShape(member.getTarget()), trait.toNode());
  }

  /** Default-value kinds the generator can express as a C++ initializer. */
  private static boolean supported(Shape target, Node value) {
    return switch (target.getType()) {
      case STRING, ENUM, BOOLEAN, BYTE, SHORT, INTEGER, LONG, INT_ENUM, FLOAT, DOUBLE, BLOB -> true;
      case TIMESTAMP -> value.isNumberNode();
      // Aggregate defaults are always empty per the spec; value-init covers them.
      case LIST, MAP -> true;
      default -> false;
    };
  }

  /**
   * The C++ expression for the member's default value ({@link #populated}/{@link #fillOnParse}
   * members only).
   */
  static String literal(CppContext context, MemberShape member) {
    Shape target = context.model().expectShape(member.getTarget());
    Node value = member.expectTrait(DefaultTrait.class).toNode();
    String type = context.cppSymbols().toSymbol(target).getName();
    return switch (target.getType()) {
      case STRING -> CppLiterals.stringLiteral(value.expectStringNode().getValue());
      case ENUM ->
          type
              + "::FromString("
              + CppLiterals.stringLiteral(value.expectStringNode().getValue())
              + ")";
      case BOOLEAN -> value.expectBooleanNode().getValue() ? "true" : "false";
      case BYTE, SHORT, INTEGER -> String.valueOf(value.expectNumberNode().getValue().longValue());
      case LONG -> CppLiterals.int64Literal(value.expectNumberNode().getValue().longValue());
      case INT_ENUM ->
          "static_cast<" + type + ">(" + value.expectNumberNode().getValue().longValue() + ")";
      case FLOAT -> "static_cast<float>(" + value.expectNumberNode().getValue().doubleValue() + ")";
      case DOUBLE -> String.valueOf(value.expectNumberNode().getValue().doubleValue());
      case TIMESTAMP ->
          "opal::Timestamp::FromEpochMilliseconds("
              + Math.round(value.expectNumberNode().getValue().doubleValue() * 1000.0)
              + "LL)";
      case BLOB ->
          "opal::Blob::FromString("
              + CppLiterals.stringLiteral(
                  new String(
                      Base64.getDecoder().decode(value.expectStringNode().getValue()),
                      java.nio.charset.StandardCharsets.ISO_8859_1))
              + ")";
      case LIST, MAP -> type + "{}";
      default ->
          throw new software.amazon.smithy.codegen.core.CodegenException(
              "cpp-codegen: unsupported @default on " + member.getId());
    };
  }
}
