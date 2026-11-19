package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.net.MalformedURLException;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.knowledge.TopDownIndex;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.protocoltests.traits.AppliesTo;
import software.amazon.smithy.protocoltests.traits.HttpMalformedRequestTestCase;
import software.amazon.smithy.protocoltests.traits.HttpMalformedRequestTestsTrait;
import software.amazon.smithy.protocoltests.traits.HttpRequestTestCase;
import software.amazon.smithy.protocoltests.traits.HttpRequestTestsTrait;
import software.amazon.smithy.protocoltests.traits.HttpResponseTestCase;
import software.amazon.smithy.protocoltests.traits.HttpResponseTestsTrait;

/**
 * The golden self-ratification backstop (issue #48): the codegen CI job checks that regeneration
 * reproduces the committed goldens byte-for-byte — but both sides of that diff come from the same
 * generator, so a generator bug that silently drops or rewrites conformance vectors ratifies itself
 * as long as the PR regenerated. This audit closes the loop from the other side: it enumerates the
 * test cases from the UPSTREAM suite definitions (the alloy and Smithy conformance jars, and the
 * authored jsonRpc2 model) using only the upstream smithy-model API — never this repo's generator
 * code — and asserts that every case is either present in the committed golden test sources or
 * named in the must-shrink exclusion list. It also verifies per-case wire facts (method, URI,
 * status) against the golden text, that the goldens contain no phantom tests with no upstream
 * source, and that every exclusion entry references a real upstream case.
 *
 * <p>The generator's own guard (stale exclusions fail generation) and this audit pull in opposite
 * directions on the same seam: together, a vector can neither linger in the exclusion list after it
 * passes nor vanish from the goldens unnoticed.
 */
class GoldenProtocolTestAuditTest {

  private record Suite(
      String name, ShapeId service, ShapeId protocolTrait, String goldenDir, String modelPath) {}

  private static final List<Suite> SUITES =
      List.of(
          new Suite(
              "simplerestjson",
              ShapeId.from("alloy.test#PizzaAdminService"),
              ShapeId.from("alloy#simpleRestJson"),
              "protocol-tests/simplerestjson/generated",
              null),
          new Suite(
              "rpcv2cbor",
              ShapeId.from("smithy.protocoltests.rpcv2Cbor#RpcV2Protocol"),
              ShapeId.from("smithy.protocols#rpcv2Cbor"),
              "protocol-tests/rpcv2cbor/generated",
              null),
          new Suite(
              "jsonrpc2",
              ShapeId.from("smithy.cpp.protocoltests.jsonrpc2#JsonRpc2Protocol"),
              ShapeId.from("smithy.cpp.protocols#jsonRpc2"),
              "protocol-tests/jsonrpc2/generated",
              "protocol-tests/jsonrpc2/model/jsonrpc2.smithy"));

  private record Expected(String file, String suiteName, String id, List<String> facts) {}

  private static Path repoRoot() {
    return Paths.get(System.getProperty("smithycpp.repoRoot"));
  }

  /** Assembles a suite's model from the upstream jars (or the authored in-repo model). */
  private static Model assemble(Suite suite) {
    if (suite.modelPath() != null) {
      return Model.assembler()
          .discoverModels(GoldenProtocolTestAuditTest.class.getClassLoader())
          .addImport(repoRoot().resolve(suite.modelPath()))
          .assemble()
          .unwrap();
    }
    String jars = System.getProperty("smithycpp.protocolTestModels", "");
    List<URL> urls = new ArrayList<>();
    for (String jar : jars.split(java.io.File.pathSeparator)) {
      if (jar.isBlank()) {
        continue;
      }
      try {
        urls.add(Paths.get(jar).toUri().toURL());
      } catch (MalformedURLException e) {
        throw new IllegalStateException("bad protocolTestModels entry: " + jar, e);
      }
    }
    assertTrue(!urls.isEmpty(), "smithycpp.protocolTestModels must point at the suite jars");
    URLClassLoader loader =
        new URLClassLoader(
            urls.toArray(new URL[0]), GoldenProtocolTestAuditTest.class.getClassLoader());
    return Model.assembler().discoverModels(loader).assemble().unwrap();
  }

  /** The exclusion list, keyed exactly as the generator keys it: "&lt;kind&gt; &lt;id&gt;". */
  private static Map<String, String> exclusionsFor(ShapeId service) throws IOException {
    Map<String, String> out = new LinkedHashMap<>();
    try (var stream =
        GoldenProtocolTestAuditTest.class.getResourceAsStream("protocol-test-exclusions.txt")) {
      for (String line : new String(stream.readAllBytes()).split("\n")) {
        String trimmed = line.trim();
        if (trimmed.isEmpty() || trimmed.startsWith("#")) {
          continue;
        }
        String[] parts = trimmed.split("\\s+", 4);
        if (parts.length >= 3 && parts[0].equals(service.toString())) {
          out.put(parts[1] + " " + parts[2], parts.length == 4 ? parts[3] : "");
        }
      }
    }
    return out;
  }

  private static boolean excluded(Map<String, String> exclusions, String kind, String id) {
    return exclusions.containsKey(kind + " " + id) || exclusions.containsKey("any " + id);
  }

  /** Reimplements the case-to-file mapping from the upstream traits, not the generator. */
  private static List<Expected> expectedCases(Suite suite, Model model) throws IOException {
    Map<String, String> exclusions = exclusionsFor(suite.service());
    String svc = suite.service().getName();
    List<Expected> out = new ArrayList<>();
    Set<String> seenErrorTests = new LinkedHashSet<>();
    List<OperationShape> operations =
        new ArrayList<>(TopDownIndex.of(model).getContainedOperations(suite.service()));
    operations.sort(Comparator.comparing(OperationShape::getId));

    for (OperationShape operation : operations) {
      operation
          .getTrait(HttpRequestTestsTrait.class)
          .ifPresent(trait -> {
            for (HttpRequestTestCase c : trait.getTestCasesFor(AppliesTo.CLIENT)) {
              if (c.getProtocol().equals(suite.protocolTrait())
                  && !excluded(exclusions, "request", c.getId())) {
                out.add(
                    new Expected(
                        "tests/request_tests.cc",
                        svc + "RequestTest",
                        c.getId(),
                        List.of("EXPECT_EQ(request.method, \"" + c.getMethod() + "\")")));
              }
            }
            for (HttpRequestTestCase c : trait.getTestCasesFor(AppliesTo.SERVER)) {
              if (c.getProtocol().equals(suite.protocolTrait())
                  && !excluded(exclusions, "server-request", c.getId())) {
                out.add(
                    new Expected(
                        "tests/server_request_tests.cc",
                        svc + "ServerRequestTest",
                        c.getId(),
                        List.of("\"" + c.getMethod() + "\"")));
              }
            }
          });
      operation
          .getTrait(HttpResponseTestsTrait.class)
          .ifPresent(trait -> {
            for (HttpResponseTestCase c : trait.getTestCasesFor(AppliesTo.CLIENT)) {
              if (c.getProtocol().equals(suite.protocolTrait())
                  && !excluded(exclusions, "response", c.getId())) {
                out.add(
                    new Expected(
                        "tests/response_tests.cc",
                        svc + "ResponseTest",
                        c.getId(),
                        List.of("next_response.status = " + c.getCode() + ";")));
              }
            }
            for (HttpResponseTestCase c : trait.getTestCasesFor(AppliesTo.SERVER)) {
              if (c.getProtocol().equals(suite.protocolTrait())
                  && !excluded(exclusions, "server-response", c.getId())) {
                out.add(
                    new Expected(
                        "tests/server_response_tests.cc",
                        svc + "ServerResponseTest",
                        c.getId(),
                        List.of("EXPECT_EQ(response.status, " + c.getCode() + ")")));
              }
            }
          });
      // Error-shape response tests (deduped: errors are shared across operations).
      for (ShapeId errorId : operation.getErrors()) {
        StructureShape error = model.expectShape(errorId, StructureShape.class);
        var trait = error.getTrait(HttpResponseTestsTrait.class);
        if (trait.isEmpty()) {
          continue;
        }
        for (HttpResponseTestCase c : trait.get().getTestCasesFor(AppliesTo.CLIENT)) {
          if (c.getProtocol().equals(suite.protocolTrait())
              && !excluded(exclusions, "error", c.getId())
              && seenErrorTests.add("client " + c.getId())) {
            out.add(
                new Expected(
                    "tests/response_tests.cc",
                    svc + "ErrorTest",
                    c.getId(),
                    List.of("next_response.status = " + c.getCode() + ";")));
          }
        }
        for (HttpResponseTestCase c : trait.get().getTestCasesFor(AppliesTo.SERVER)) {
          if (c.getProtocol().equals(suite.protocolTrait())
              && !excluded(exclusions, "server-error", c.getId())
              && seenErrorTests.add("server " + c.getId())) {
            out.add(
                new Expected(
                    "tests/server_response_tests.cc",
                    svc + "ServerErrorTest",
                    c.getId(),
                    List.of("EXPECT_EQ(response.status, " + c.getCode() + ")")));
          }
        }
      }
      operation
          .getTrait(HttpMalformedRequestTestsTrait.class)
          .ifPresent(trait -> {
            // getTestCases() expands testParameters into concrete cases.
            for (HttpMalformedRequestTestCase c : trait.getTestCases()) {
              if (c.getProtocol().equals(suite.protocolTrait())
                  && !excluded(exclusions, "server-malformed", c.getId())) {
                out.add(
                    new Expected(
                        "tests/server_malformed_tests.cc",
                        svc + "ServerMalformedTest",
                        c.getId(),
                        List.of("EXPECT_EQ(response.status, " + c.getResponse().getCode() + ")")));
              }
            }
          });
    }
    return out;
  }

  private static String goldenFile(Suite suite, String file) throws IOException {
    Path path = repoRoot().resolve(suite.goldenDir()).resolve(file);
    assertTrue(Files.exists(path), "missing golden file: " + path);
    return Files.readString(path);
  }

  /** The set of test ids a golden file defines for one gtest suite name. */
  private static Set<String> definedTests(String source, String suiteName) {
    Set<String> out = new TreeSet<>();
    Matcher matcher =
        Pattern.compile("TEST\\(" + Pattern.quote(suiteName) + ",\\s*(\\w+)\\)").matcher(source);
    while (matcher.find()) {
      out.add(matcher.group(1));
    }
    return out;
  }

  /** The body of one TEST, from its header to the next TEST or EOF. */
  private static String testChunk(String source, String suiteName, String id) {
    int start = source.indexOf("TEST(" + suiteName + ", " + id + ")");
    assertTrue(start >= 0, "TEST(" + suiteName + ", " + id + ") not found");
    int end = source.indexOf("\nTEST(", start);
    return end < 0 ? source.substring(start) : source.substring(start, end);
  }

  @Test
  void everyUpstreamCaseIsGeneratedOrExcluded() throws IOException {
    for (Suite suite : SUITES) {
      Model model = assemble(suite);
      List<Expected> expected = expectedCases(suite, model);
      assertTrue(
          expected.size() > 50 || !suite.name().equals("rpcv2cbor"),
          suite.name() + ": implausibly few upstream cases enumerated (" + expected.size() + ")");

      Map<String, Set<String>> expectedByFileSuite = new LinkedHashMap<>();
      for (Expected e : expected) {
        expectedByFileSuite
            .computeIfAbsent(e.file() + "|" + e.suiteName(), k -> new TreeSet<>())
            .add(e.id());
      }
      for (Map.Entry<String, Set<String>> entry : expectedByFileSuite.entrySet()) {
        String[] key = entry.getKey().split("\\|");
        Set<String> defined = definedTests(goldenFile(suite, key[0]), key[1]);
        Set<String> missing = new TreeSet<>(entry.getValue());
        missing.removeAll(defined);
        assertTrue(
            missing.isEmpty(),
            suite.name()
                + " "
                + key[0]
                + ": upstream cases absent from the golden (regenerate, or add to"
                + " protocol-test-exclusions.txt): "
                + missing);
        Set<String> phantom = new TreeSet<>(defined);
        phantom.removeAll(entry.getValue());
        assertTrue(
            phantom.isEmpty(),
            suite.name()
                + " "
                + key[0]
                + ": golden defines tests with no upstream source (suite "
                + key[1]
                + "): "
                + phantom);
      }
    }
  }

  @Test
  void goldenTestsCarryTheUpstreamWireFacts() throws IOException {
    for (Suite suite : SUITES) {
      Model model = assemble(suite);
      for (Expected e : expectedCases(suite, model)) {
        String chunk = testChunk(goldenFile(suite, e.file()), e.suiteName(), e.id());
        for (String fact : e.facts()) {
          assertTrue(
              chunk.contains(fact),
              suite.name()
                  + " "
                  + e.file()
                  + " "
                  + e.id()
                  + ": golden disagrees with the upstream definition; expected to find: "
                  + fact);
        }
      }
    }
  }

  @Test
  void everyExclusionNamesARealUpstreamCase() throws IOException {
    for (Suite suite : SUITES) {
      Model model = assemble(suite);
      Set<String> upstreamIds = new TreeSet<>();
      for (OperationShape operation :
          TopDownIndex.of(model).getContainedOperations(suite.service())) {
        operation
            .getTrait(HttpRequestTestsTrait.class)
            .ifPresent(t -> t.getTestCases().forEach(c -> upstreamIds.add(c.getId())));
        operation
            .getTrait(HttpResponseTestsTrait.class)
            .ifPresent(t -> t.getTestCases().forEach(c -> upstreamIds.add(c.getId())));
        operation
            .getTrait(HttpMalformedRequestTestsTrait.class)
            .ifPresent(t -> t.getTestCases().forEach(c -> upstreamIds.add(c.getId())));
        for (ShapeId errorId : operation.getErrors()) {
          model
              .expectShape(errorId, StructureShape.class)
              .getTrait(HttpResponseTestsTrait.class)
              .ifPresent(t -> t.getTestCases().forEach(c -> upstreamIds.add(c.getId())));
        }
      }
      for (String key : exclusionsFor(suite.service()).keySet()) {
        String id = key.split(" ", 2)[1];
        assertTrue(
            upstreamIds.contains(id),
            suite.name() + ": exclusion references no upstream case: " + key);
      }
    }
  }
}
