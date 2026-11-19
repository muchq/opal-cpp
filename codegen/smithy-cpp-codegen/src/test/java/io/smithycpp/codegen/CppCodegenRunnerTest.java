package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import software.amazon.smithy.codegen.core.CodegenException;

/**
 * Pins the runner's --omit-operation contract (issue #47): omits prune unsupported operations, and
 * the list self-polices — an omit whose operation the generator NOW supports fails the run instead
 * of silently staying excluded forever ("must only shrink").
 */
class CppCodegenRunnerTest {

  // The Bad* inputs reach recursion through a union member, which the
  // generator rejects at the door (the same limitation
  // CppCodegenPluginTest.rejectsRecursionThroughUnionMembers pins) — the
  // archetypal "unsupported feature" omit target.
  private static final String MODEL =
      """
      $version: "2.0"
      namespace test.omit

      service Svc { version: "1", operations: [Good, Bad] }
      operation Good { input := { name: String } }
      operation Bad { input := { tree: TreeNode } }

      structure TreeNode {
          value: TreeValue
      }
      union TreeValue {
          leaf: String
          node: TreeNode
      }
      """;

  // Two unsupported operations sharing the recursive shape: each probe must
  // keep the OTHER omit applied or it fails for the wrong reason.
  private static final String TWO_BAD_MODEL =
      """
      $version: "2.0"
      namespace test.omit

      service Svc { version: "1", operations: [Good, Bad, AlsoBad] }
      operation Good { input := { name: String } }
      operation Bad { input := { tree: TreeNode } }
      operation AlsoBad { input := { other: TreeNode } }

      structure TreeNode {
          value: TreeValue
      }
      union TreeValue {
          leaf: String
          node: TreeNode
      }
      """;

  @TempDir Path tmp;

  private String[] args(Path modelFile, Path output, String... omitOperations) {
    List<String> argv =
        new ArrayList<>(
            List.of(
                "--model",
                modelFile.toString(),
                "--service",
                "test.omit#Svc",
                "--namespace",
                "test::omit",
                "--output",
                output.toString()));
    for (String op : omitOperations) {
      argv.addAll(List.of("--omit-operation", op));
    }
    return argv.toArray(new String[0]);
  }

  private Path writeModel(String model) throws IOException {
    Path modelFile = tmp.resolve("omit.smithy");
    Files.writeString(modelFile, model);
    return modelFile;
  }

  @Test
  void validOmitPrunesTheUnsupportedOperation() throws IOException {
    Path output = tmp.resolve("out-valid");
    CppCodegenRunner.run(args(writeModel(MODEL), output, "test.omit#Bad"));
    String header = Files.readString(output.resolve("include/test/omit/types.h"));
    assertTrue(header.contains("struct GoodInput"), header);
    // The omitted operation and everything only it referenced are gone.
    assertFalse(header.contains("TreeNode"), header);
  }

  @Test
  void staleOmitOfASupportedOperationFailsTheRun() throws IOException {
    Path output = tmp.resolve("out-stale");
    // Good generates cleanly, so omitting it is stale; Bad's omit is still
    // legitimate (and keeps Good's probe from failing for unrelated reasons).
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () ->
                CppCodegenRunner.run(
                    args(writeModel(MODEL), output, "test.omit#Bad", "test.omit#Good")));
    assertTrue(
        error.getMessage().contains("stale --omit-operation test.omit#Good"), error.getMessage());
    assertTrue(error.getMessage().contains("must only shrink"), error.getMessage());
  }

  @Test
  void probesKeepTheOtherOmitsAppliedSoNeitherReadsAsStale() throws IOException {
    // Bad and AlsoBad share the recursive shape: each probe retains one and
    // prunes the other, and both must still fail (recursion) — omitting both
    // together generates cleanly with no false stale report for either.
    Path output = tmp.resolve("out-attributable");
    assertDoesNotThrow(() ->
        CppCodegenRunner.run(
            args(writeModel(TWO_BAD_MODEL), output, "test.omit#Bad", "test.omit#AlsoBad")));
    String header = Files.readString(output.resolve("include/test/omit/types.h"));
    assertTrue(header.contains("struct GoodInput"), header);
    assertFalse(header.contains("TreeNode"), header);
  }

  @Test
  void unknownOmitIdStillFailsFast() throws IOException {
    Path output = tmp.resolve("out-unknown");
    assertThrows(
        RuntimeException.class,
        () -> CppCodegenRunner.run(args(writeModel(MODEL), output, "test.omit#NoSuchOp")));
  }

  @Test
  void unknownArgumentIsAnAttributedCodegenDiagnostic() {
    CodegenException error =
        assertThrows(
            CodegenException.class, () -> CppCodegenRunner.run(new String[] {"--bogus", "x"}));
    assertTrue(
        error.getMessage().startsWith("cpp-codegen: unknown argument --bogus"), error.getMessage());
  }

  @Test
  void missingRequiredArgumentsAreAnAttributedCodegenDiagnostic() {
    CodegenException error =
        assertThrows(
            CodegenException.class,
            () -> CppCodegenRunner.run(new String[] {"--service", "test.omit#Svc"}));
    assertTrue(error.getMessage().startsWith("cpp-codegen: required:"), error.getMessage());
    assertTrue(error.getMessage().contains("--namespace"), error.getMessage());
  }

  @Test
  void describeKeepsTheAttributedMessageAndFormatsValidationEvents() throws IOException {
    // What the Bazel action log shows (issue #49): the diagnostic itself,
    // not a stack trace. CodegenExceptions pass through; a model that fails
    // Smithy validation gets the prefix plus one line per event.
    assertTrue(
        CppCodegenRunner.describe(new CodegenException("cpp-codegen: boom"))
            .equals("cpp-codegen: boom"));
    Path bad = tmp.resolve("bad.smithy");
    Files.writeString(
        bad,
        """
        $version: "2.0"
        namespace test.bad

        structure Broken {
            member: test.bad#DoesNotExist
        }
        """);
    var thrown =
        assertThrows(
            software.amazon.smithy.model.validation.ValidatedResultException.class,
            () ->
                CppCodegenRunner.run(
                    new String[] {
                      "--model",
                      bad.toString(),
                      "--service",
                      "test.bad#Nope",
                      "--namespace",
                      "test::bad",
                      "--output",
                      tmp.resolve("out-bad").toString()
                    }));
    String described = CppCodegenRunner.describe(thrown);
    assertTrue(described.startsWith("cpp-codegen: the model failed Smithy validation:"), described);
    assertTrue(described.contains("DoesNotExist"), described);
    assertFalse(described.contains("at software.amazon"), described); // no stack frames
  }
}
