package io.smithycpp.codegen;

import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import software.amazon.smithy.build.FileManifest;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.node.Node;
import software.amazon.smithy.model.node.ObjectNode;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.transform.ModelTransformer;

/**
 * Command-line entry point used by the {@code generateFixtures} / {@code generateProtocolTests}
 * Gradle tasks (and anyone who wants generation without smithy-build):
 *
 * <pre>
 * CppCodegenRunner [--model M.smithy]... --service NS#Svc --namespace a::b \
 *     --runtime-target //runtime:core --output DIR \
 *     [--tests-package //pkg] [--omit-operation NS#Op]... [--malformed-tests true]
 * </pre>
 *
 * <p>Models are discovered from the classpath (so the official protocol-test suites generate
 * straight from their published jars); {@code --model} adds a local file on top. {@code
 * --omit-operation} prunes operations that use features the generator doesn't support yet — the
 * generated module documents the gap, and the flag list must only shrink over time. That shrink is
 * enforced: each omitted operation is probe-generated in memory, and an omit whose operation now
 * generates cleanly fails the run until the flag is removed (mirroring the self-policing
 * protocol-test exclusion list).
 */
public final class CppCodegenRunner {

  private CppCodegenRunner() {}

  public static void main(String[] args) {
    try {
      run(args);
    } catch (CodegenException
        | software.amazon.smithy.model.validation.ValidatedResultException e) {
      // The most common consumer mistake — an invalid model or bad flag —
      // surfaces through the Bazel rule's action log. Print the diagnostic
      // itself instead of burying it in a JVM stack trace (issue #49); real
      // generator bugs (any other exception) still get the full trace.
      System.err.println(describe(e));
      System.exit(1);
    }
  }

  /**
   * One line per problem, each carrying the {@code cpp-codegen:} attribution so the message is
   * findable in a Bazel action log. Package-private for tests (main exits the JVM).
   */
  static String describe(RuntimeException e) {
    if (e instanceof software.amazon.smithy.model.validation.ValidatedResultException validation) {
      StringBuilder out = new StringBuilder("cpp-codegen: the model failed Smithy validation:");
      for (var event : validation.getValidationEvents()) {
        out.append(System.lineSeparator()).append("  ").append(event);
      }
      return out.toString();
    }
    return e.getMessage();
  }

  /** The runner body; throws instead of exiting so tests and probes can assert on failures. */
  static void run(String[] args) {
    List<String> modelPaths = new ArrayList<>();
    String service = null;
    String namespace = null;
    String runtimeTarget = "@opal_cpp//runtime:core";
    String output = null;
    String testsPackage = null;
    boolean malformedTests = false;
    boolean integrationTests = false;
    String mode = null;
    Boolean emitBuildFile = null;
    List<String> omitOperations = new ArrayList<>();
    for (int i = 0; i + 1 < args.length; i += 2) {
      switch (args[i]) {
        case "--model" -> modelPaths.add(args[i + 1]);
        case "--service" -> service = args[i + 1];
        case "--namespace" -> namespace = args[i + 1];
        case "--runtime-target" -> runtimeTarget = args[i + 1];
        case "--output" -> output = args[i + 1];
        case "--tests-package" -> testsPackage = args[i + 1];
        case "--omit-operation" -> omitOperations.add(args[i + 1]);
        case "--malformed-tests" -> malformedTests = Boolean.parseBoolean(args[i + 1]);
        case "--integration-tests" -> integrationTests = Boolean.parseBoolean(args[i + 1]);
        case "--mode" -> mode = args[i + 1];
        case "--emit-build-file" -> emitBuildFile = Boolean.parseBoolean(args[i + 1]);
        default ->
            throw new CodegenException(
                "cpp-codegen: unknown argument "
                    + args[i]
                    + " (see the CppCodegenRunner javadoc for the accepted flags)");
      }
    }
    if (service == null || namespace == null || output == null) {
      throw new CodegenException(
          "cpp-codegen: required: --service <id> --namespace <ns> --output <dir>"
              + " [--model <file>]");
    }

    var assembler = Model.assembler().discoverModels(CppCodegenRunner.class.getClassLoader());
    for (String modelPath : modelPaths) {
      assembler.addImport(Paths.get(modelPath));
    }
    Model model = assembler.assemble().unwrap();

    ObjectNode.Builder settings =
        Node.objectNodeBuilder()
            .withMember("service", service)
            .withMember("namespace", namespace)
            .withMember("runtimeTarget", runtimeTarget);
    if (testsPackage != null) {
      settings.withMember("testsPackage", testsPackage);
    }
    if (malformedTests) {
      settings.withMember("malformedTests", true);
    }
    if (integrationTests) {
      settings.withMember("integrationTests", true);
    }
    if (mode != null) {
      settings.withMember("mode", mode);
    }
    if (emitBuildFile != null) {
      settings.withMember("emitBuildFile", emitBuildFile);
    }
    ObjectNode settingsNode = settings.build();

    if (!omitOperations.isEmpty()) {
      rejectStaleOmits(model, omitOperations, settingsNode);
      model = pruneOperations(model, omitOperations);
    }

    runPlugin(FileManifest.create(Paths.get(output)), model, settingsNode);
  }

  /** Runs the plugin once — the real run and the staleness probes share this wiring. */
  private static void runPlugin(FileManifest manifest, Model model, ObjectNode settings) {
    PluginContext context =
        PluginContext.builder().fileManifest(manifest).model(model).settings(settings).build();
    new CppCodegenPlugin().execute(context);
  }

  /** Removes the named operations (and everything only they referenced) from the model. */
  private static Model pruneOperations(Model model, Collection<String> operationIds) {
    if (operationIds.isEmpty()) {
      return model;
    }
    Set<Shape> toRemove = new HashSet<>();
    for (String id : operationIds) {
      toRemove.add(model.expectShape(ShapeId.from(id)));
    }
    ModelTransformer transformer = ModelTransformer.create();
    Model pruned = transformer.removeShapes(model, toRemove);
    return transformer.removeUnreferencedShapes(pruned);
  }

  /**
   * Enforces the must-only-shrink contract on {@code --omit-operation}: each omitted operation is
   * probe-generated in memory (with the other omits still applied, so a failure is attributable to
   * it alone), and an omit whose operation now generates cleanly fails the run — otherwise a newly
   * supported operation would silently stay excluded from its suite forever.
   */
  private static void rejectStaleOmits(
      Model model, List<String> omitOperations, ObjectNode settings) {
    for (String id : omitOperations) {
      model.expectShape(ShapeId.from(id));
    }
    for (String id : omitOperations) {
      List<String> others = new ArrayList<>(omitOperations);
      others.remove(id);
      try {
        runPlugin(new MockManifest(), pruneOperations(model, others), settings);
      } catch (CodegenException stillUnsupported) {
        // Only the generator's own "can't handle this" signal keeps an omit.
        // Anything else is a real bug that must surface here — this probe is
        // the one execution that ever generates the omitted operation.
        continue;
      }
      throw new CodegenException(
          "cpp-codegen: stale --omit-operation "
              + id
              + ": generation now succeeds with the operation included — remove the flag"
              + " (the omit list must only shrink)");
    }
  }
}
