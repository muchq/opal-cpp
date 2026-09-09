package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.ObjectNode;

class CppSettingsTest {

  private static ObjectNode minimal() {
    return Node.objectNodeBuilder()
        .withMember("service", "example.weather#Weather")
        .withMember("namespace", "example::weather")
        .build();
  }

  @Test
  void defaultsApplyWhenOnlyServiceAndNamespaceAreGiven() {
    CppSettings settings = CppSettings.fromNode(minimal());
    assertEquals("example.weather#Weather", settings.service().toString());
    assertEquals("example::weather", settings.namespace());
    assertEquals("@opal_cpp//runtime:core", settings.runtimeTarget());
  }

  @Test
  void pathsDeriveFromTheNamespace() {
    CppSettings settings = CppSettings.fromNode(minimal());
    assertEquals("example/weather", settings.includePrefix());
    assertEquals("include/example/weather/types.h", settings.typesHeaderFile());
    assertEquals("include/example/weather/serde.h", settings.serdeHeaderFile());
    assertEquals("include/example/weather/client.h", settings.clientHeaderFile());
    assertEquals("include/example/weather/server.h", settings.serverHeaderFile());
  }

  @Test
  void runtimePackageStripsTheTargetName() {
    ObjectNode node = minimal().withMember("runtimeTarget", "//runtime:core");
    assertEquals("//runtime", CppSettings.fromNode(node).runtimePackage());
  }

  @Test
  void rejectsAnUnknownMode() {
    ObjectNode node = minimal().withMember("mode", "banana");
    CodegenException thrown =
        assertThrows(CodegenException.class, () -> CppSettings.fromNode(node));
    assertTrue(thrown.getMessage().contains("banana"));
  }

  @Test
  void rejectsANamespaceThatIsNotCpp() {
    // The namespace flows into emitted `namespace a::b {` blocks and include
    // paths; anything else must fail at settings time, not compile time.
    for (String bad : new String[] {"example.weather", "1abc", "a::", "::a", "a b", ""}) {
      ObjectNode node =
          Node.objectNodeBuilder()
              .withMember("service", "example.weather#Weather")
              .withMember("namespace", bad)
              .build();
      assertThrows(CodegenException.class, () -> CppSettings.fromNode(node), "namespace: " + bad);
    }
  }

  @Test
  void missingServiceOrNamespaceFailsLoudly() {
    assertThrows(
        Exception.class,
        () ->
            CppSettings.fromNode(Node.objectNodeBuilder().withMember("namespace", "a::b").build()));
    assertThrows(
        Exception.class,
        () -> CppSettings.fromNode(Node.objectNodeBuilder().withMember("service", "a#B").build()));
  }
}
