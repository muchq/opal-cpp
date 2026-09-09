package io.smithycpp.codegen;

import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.build.SmithyBuildPlugin;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.codegen.core.directed.CodegenDirector;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.neighbor.Walker;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Entry point for C++ code generation, registered with smithy-build as the {@code cpp-codegen}
 * plugin. Phase 2 scope: data type generation (see docs/PLAN.md).
 */
public final class CppCodegenPlugin implements SmithyBuildPlugin {

  @Override
  public String getName() {
    return "cpp-codegen";
  }

  @Override
  public void execute(PluginContext context) {
    CppSettings settings = CppSettings.fromNode(context.getSettings());
    rejectUnsupportedRecursion(context.getModel(), settings.service());

    CodegenDirector<CppWriter, CppIntegration, CppContext, CppSettings> runner =
        new CodegenDirector<>();
    runner.directedCodegen(new DirectedCppCodegen());
    runner.integrationClass(CppIntegration.class);
    runner.fileManifest(context.getFileManifest());
    runner.model(context.getModel());
    runner.settings(settings);
    runner.service(settings.service());
    runner.performDefaultCodegenTransforms();
    // Legacy string shapes carrying @enum become real enum shapes, so every
    // enum downstream (types, serde, validation) has one representation.
    runner.changeStringEnumsToEnumShapes(true);
    runner.createDedicatedInputsAndOutputs();
    runner.run();
  }

  /**
   * Recursive structure members are supported via opal::Boxed (and lists via std::vector's
   * incomplete-element support), but cycles through union members or map values still need
   * representation work; fail generation with a clear message instead of emitting non-compiling
   * code.
   */
  private static void rejectUnsupportedRecursion(Model model, ShapeId service) {
    RecursionIndex recursion = new RecursionIndex(model);
    Walker walker = new Walker(model);
    for (Shape shape : walker.walkShapes(model.expectShape(service))) {
      for (var member : shape.members()) {
        String reason = recursion.unsupportedCycleMember(member);
        if (reason != null) {
          throw new CodegenException("cpp-codegen: " + reason);
        }
      }
    }
  }
}
