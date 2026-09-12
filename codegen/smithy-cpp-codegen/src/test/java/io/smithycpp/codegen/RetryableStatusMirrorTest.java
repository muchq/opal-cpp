package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;

/**
 * The generator refuses a @streaming response payload whose modeled success status is one the retry
 * layer treats as transient (issue #213 slice 2), which means the Java side carries a second copy
 * of a set whose source of truth is C++: {@code opal::RetryableStatus} in
 * runtime/src/client/retry.cc. Two copies drift, and the drift would be silent in the direction
 * that matters — a status added there but not here goes back to generating a writer the retry layer
 * can never invoke.
 *
 * <p>So this reads the C++ and compares. The same self-policing style as {@link
 * DiagnosticConventionTest}: the mirror is allowed to exist because a test fails when it stops
 * being one.
 */
class RetryableStatusMirrorTest {

  private static final Path RETRY_CC =
      Paths.get(System.getProperty("smithycpp.repoRoot"), "runtime/src/client/retry.cc");

  /** The status literals in RetryableStatus's one-line body. */
  private static final Pattern BODY =
      Pattern.compile("bool RetryableStatus\\(int status\\) \\{\\s*return([^}]*)\\}");

  @Test
  void theGeneratorsRetryableStatusesAreTheRuntimes() throws IOException {
    String text = Files.readString(RETRY_CC);
    Matcher matcher = BODY.matcher(text);
    org.junit.jupiter.api.Assertions.assertTrue(
        matcher.find(), "RetryableStatus(int status) not found in " + RETRY_CC);

    Set<Integer> runtime = new TreeSet<>();
    Matcher statuses = Pattern.compile("status == (\\d{3})").matcher(matcher.group(1));
    while (statuses.find()) {
      runtime.add(Integer.parseInt(statuses.group(1)));
    }

    assertEquals(
        new TreeSet<>(HttpBindingCodeGen.RETRYABLE_STATUSES),
        runtime,
        "HttpBindingCodeGen.RETRYABLE_STATUSES has drifted from opal::RetryableStatus in "
            + RETRY_CC);
  }
}
