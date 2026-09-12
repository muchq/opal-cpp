package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

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

  /** RetryableStatus's one-line body. */
  private static final Pattern BODY =
      Pattern.compile("bool RetryableStatus\\(int status\\) \\{\\s*return([^}]*)\\}");

  /** One status literal compared against `status` inside that body. */
  private static final Pattern STATUS = Pattern.compile("status == (\\d{3})");

  @Test
  void theGeneratorsRetryableStatusesAreTheRuntimes() throws IOException {
    String text = Files.readString(RETRY_CC);
    Matcher matcher = BODY.matcher(text);
    assertTrue(matcher.find(), "RetryableStatus(int status) not found in " + RETRY_CC);

    // Compared as the digit tokens both sides are written with, rather than as
    // parsed ints: the comparison is exact either way, and there is no parse to
    // fail on input the pattern has already constrained to three digits.
    Set<String> runtime = new TreeSet<>();
    Matcher statuses = STATUS.matcher(matcher.group(1));
    while (statuses.find()) {
      runtime.add(statuses.group(1));
    }
    assertFalse(runtime.isEmpty(), "no status literals found in RetryableStatus in " + RETRY_CC);

    Set<String> generator = new TreeSet<>();
    for (Integer status : HttpBindingCodeGen.RETRYABLE_STATUSES) {
      generator.add(String.valueOf(status));
    }

    assertEquals(
        generator,
        runtime,
        "HttpBindingCodeGen.RETRYABLE_STATUSES has drifted from opal::RetryableStatus in "
            + RETRY_CC);
  }
}
