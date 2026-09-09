package io.smithycpp.codegen;

import java.util.Objects;
import software.amazon.smithy.model.node.ObjectNode;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Settings for the {@code cpp-codegen} smithy-build plugin.
 *
 * <pre>{@code
 * "cpp-codegen": {
 *   "service": "example.weather#Weather",
 *   "namespace": "example::weather",
 *   "runtimeTarget": "@opal_cpp//runtime:core"
 * }
 * }</pre>
 */
public final class CppSettings {

  private final ShapeId service;
  private final String namespace;
  private final String runtimeTarget;
  private final String testsPackage;
  private final boolean malformedTests;
  private final boolean integrationTests;
  private final String mode;
  private final boolean emitBuildFile;

  private CppSettings(
      ShapeId service,
      String namespace,
      String runtimeTarget,
      String testsPackage,
      boolean malformedTests,
      boolean integrationTests,
      String mode,
      boolean emitBuildFile) {
    this.service = service;
    this.namespace = namespace;
    this.runtimeTarget = runtimeTarget;
    this.testsPackage = testsPackage;
    this.malformedTests = malformedTests;
    this.integrationTests = integrationTests;
    this.mode = mode;
    this.emitBuildFile = emitBuildFile;
  }

  public static CppSettings fromNode(ObjectNode node) {
    ShapeId service = ShapeId.from(node.expectStringMember("service").getValue());
    String namespace = node.expectStringMember("namespace").getValue();
    String runtimeTarget =
        node.getStringMemberOrDefault("runtimeTarget", "@opal_cpp//runtime:core");
    String testsPackage = node.getStringMemberOrDefault("testsPackage", null);
    boolean malformedTests = node.getBooleanMemberOrDefault("malformedTests", false);
    boolean integrationTests = node.getBooleanMemberOrDefault("integrationTests", false);
    String mode = node.getStringMemberOrDefault("mode", "both");
    boolean emitBuildFile = node.getBooleanMemberOrDefault("emitBuildFile", true);
    if (!mode.matches("types|client|server|both")) {
      throw new software.amazon.smithy.codegen.core.CodegenException(
          "cpp-codegen: 'mode' must be types, client, server, or both, got: " + mode);
    }
    if (!namespace.matches("[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*")) {
      throw new software.amazon.smithy.codegen.core.CodegenException(
          "cpp-codegen: 'namespace' must be a C++ namespace like a::b, got: " + namespace);
    }
    return new CppSettings(
        service,
        namespace,
        runtimeTarget,
        testsPackage,
        malformedTests,
        integrationTests,
        mode,
        emitBuildFile);
  }

  public ShapeId service() {
    return service;
  }

  /** C++ namespace, e.g. {@code example::weather}. */
  public String namespace() {
    return namespace;
  }

  /** Bazel label of the smithy-cpp runtime core library the generated code depends on. */
  public String runtimeTarget() {
    return runtimeTarget;
  }

  /** Include-path prefix derived from the namespace, e.g. {@code example/weather}. */
  public String includePrefix() {
    return namespace.replace("::", "/");
  }

  /** Repo-relative path of the generated types header. */
  public String typesHeaderFile() {
    return "include/" + includePrefix() + "/types.h";
  }

  public String serdeHeaderFile() {
    return "include/" + includePrefix() + "/serde.h";
  }

  public String clientHeaderFile() {
    return "include/" + includePrefix() + "/client.h";
  }

  public String serverHeaderFile() {
    return "include/" + includePrefix() + "/server.h";
  }

  /** Bazel package of the runtime, e.g. {@code //runtime} or {@code @opal_cpp//runtime}. */
  public String runtimePackage() {
    int colon = runtimeTarget.lastIndexOf(':');
    return colon < 0 ? runtimeTarget : runtimeTarget.substring(0, colon);
  }

  /**
   * Bazel package of the generated module (e.g. {@code //examples/weather/generated}); when set,
   * tests are generated into {@code tests/}: service smoke tests always, protocol conformance tests
   * when the model carries smithy.test traits. Null otherwise.
   */
  public String testsPackage() {
    return testsPackage;
  }

  /**
   * Whether to generate server suites from {@code smithy.test#httpMalformedRequestTests}. Off by
   * default: most such suites exercise parser strictness the generator doesn't enforce yet, so it's
   * enabled per-module (the constraint-validation suite) rather than globally.
   */
  public boolean malformedTests() {
    return malformedTests;
  }

  /**
   * Whether to generate tests/integration_test.cc: generated client vs generated server over
   * loopback and real sockets with random round-trips (PLAN Phase 5). Per-module, like the
   * conformance suites.
   */
  public boolean integrationTests() {
    return integrationTests;
  }

  /** What to generate: types | client | server | both (the Bazel rules pick one per target). */
  public String mode() {
    return mode;
  }

  public boolean generateClient() {
    return mode.equals("client") || mode.equals("both");
  }

  public boolean generateServer() {
    return mode.equals("server") || mode.equals("both");
  }

  /** Off when generation runs as a Bazel action (the consumer wrote the BUILD file). */
  public boolean emitBuildFile() {
    return emitBuildFile;
  }

  @Override
  public boolean equals(Object other) {
    if (!(other instanceof CppSettings that)) {
      return false;
    }
    return service.equals(that.service)
        && namespace.equals(that.namespace)
        && runtimeTarget.equals(that.runtimeTarget)
        && Objects.equals(testsPackage, that.testsPackage)
        && malformedTests == that.malformedTests
        && integrationTests == that.integrationTests
        && mode.equals(that.mode)
        && emitBuildFile == that.emitBuildFile;
  }

  @Override
  public int hashCode() {
    return Objects.hash(
        service,
        namespace,
        runtimeTarget,
        testsPackage,
        malformedTests,
        integrationTests,
        mode,
        emitBuildFile);
  }
}
