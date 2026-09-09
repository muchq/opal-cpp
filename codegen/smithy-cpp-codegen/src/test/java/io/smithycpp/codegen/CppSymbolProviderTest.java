package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.ShapeId;

class CppSymbolProviderTest {

  private static final String PREAMBLE =
      """
      $version: "2.0"
      namespace test.smithy

      """;

  private static Model model(String body) {
    return Model.assembler().addUnparsedModel("test.smithy", PREAMBLE + body).assemble().unwrap();
  }

  private static CppSymbolProvider provider(Model model) {
    return new CppSymbolProvider(
        model,
        CppSettings.fromNode(
            Node.objectNodeBuilder()
                .withMember("service", "test.smithy#Svc")
                .withMember("namespace", "test::smithy")
                .build()));
  }

  @Test
  void mapsSimpleShapesToCppTypes() {
    Model m =
        model(
            """
            structure Sample {
                text: String
                @required
                flag: Boolean
                count: Integer
                big: Long
                ratio: Double
                data: Blob
                at: Timestamp
                any: Document
            }
            list Texts { member: String }
            map Amounts { key: String, value: Integer }
            """);
    CppSymbolProvider symbols = provider(m);
    assertEquals(
        "std::vector<std::string>",
        symbols.toSymbol(m.expectShape(ShapeId.from("test.smithy#Texts"))).getName());
    assertEquals(
        "std::map<std::string, std::int32_t>",
        symbols.toSymbol(m.expectShape(ShapeId.from("test.smithy#Amounts"))).getName());

    MemberShape flag =
        m.expectShape(ShapeId.from("test.smithy#Sample$flag")).asMemberShape().orElseThrow();
    assertEquals("bool", symbols.toMemberSymbol(flag).getName());
    MemberShape text =
        m.expectShape(ShapeId.from("test.smithy#Sample$text")).asMemberShape().orElseThrow();
    assertEquals("std::optional<std::string>", symbols.toMemberSymbol(text).getName());
    assertTrue(CppSymbolProvider.headersOf(symbols.toMemberSymbol(text)).contains("<optional>"));
    MemberShape data =
        m.expectShape(ShapeId.from("test.smithy#Sample$data")).asMemberShape().orElseThrow();
    assertTrue(
        CppSymbolProvider.headersOf(symbols.toMemberSymbol(data)).contains("\"opal/core/blob.h\""));
  }

  @Test
  void sparseCollectionsWrapElementsInOptional() {
    Model m =
        model(
            """
            @sparse
            list MaybeTexts { member: String }
            """);
    assertEquals(
        "std::vector<std::optional<std::string>>",
        provider(m).toSymbol(m.expectShape(ShapeId.from("test.smithy#MaybeTexts"))).getName());
  }

  @Test
  void escapesReservedWords() {
    Model m =
        model(
            """
            structure Sample {
                namespace: String
                operator: Integer
                plain: String
            }
            """);
    CppSymbolProvider symbols = provider(m);
    MemberShape ns =
        m.expectShape(ShapeId.from("test.smithy#Sample$namespace")).asMemberShape().orElseThrow();
    assertEquals("namespace_", symbols.toMemberName(ns));
    MemberShape op =
        m.expectShape(ShapeId.from("test.smithy#Sample$operator")).asMemberShape().orElseThrow();
    assertEquals("operator_", symbols.toMemberName(op));
    MemberShape plain =
        m.expectShape(ShapeId.from("test.smithy#Sample$plain")).asMemberShape().orElseThrow();
    assertEquals("plain", symbols.toMemberName(plain));
  }

  @Test
  void escapesTheGeneratedFileLevelIdentifiersOnTypesOnly() {
    // Generated .cc files claim `helpers` (the file-local helper namespace)
    // and `types` (the model-type alias), so a TYPE with either name gets the
    // keyword treatment — while members keep the plain spelling, since member
    // access never collides with a namespace (issue #71).
    Model m =
        model(
            """
            structure helpers {
                helpers: String
            }
            structure types {
                types: String
            }
            """);
    CppSymbolProvider symbols = provider(m);
    assertEquals(
        "helpers_", symbols.declaredName(m.expectShape(ShapeId.from("test.smithy#helpers"))));
    assertEquals("types_", symbols.declaredName(m.expectShape(ShapeId.from("test.smithy#types"))));
    MemberShape helpersMember =
        m.expectShape(ShapeId.from("test.smithy#helpers$helpers")).asMemberShape().orElseThrow();
    assertEquals("helpers", symbols.toMemberName(helpersMember));
    MemberShape typesMember =
        m.expectShape(ShapeId.from("test.smithy#types$types")).asMemberShape().orElseThrow();
    assertEquals("types", symbols.toMemberName(typesMember));
  }

  @Test
  void rejectsUnsupportedShapesWithClearMessage() {
    Model m = model("bigDecimal Money");
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () -> provider(m).toSymbol(m.expectShape(ShapeId.from("test.smithy#Money"))));
    assertTrue(error.getMessage().contains("bigDecimal"));
    assertTrue(error.getMessage().contains("test.smithy#Money"));
  }

  @Test
  void enumConstantNaming() {
    assertEquals("kDrip", TypeGenerators.enumConstant("DRIP"));
    assertEquals("kOatMilk", TypeGenerators.enumConstant("OAT_MILK"));
    assertEquals("kMixedCase", TypeGenerators.enumConstant("mixed-CASE"));
  }
}
