package io.smithycpp.codegen;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.protocoltests.traits.AppliesTo;
import software.amazon.smithy.protocoltests.traits.HttpMalformedRequestTestCase;
import software.amazon.smithy.protocoltests.traits.HttpMalformedRequestTestsTrait;
import software.amazon.smithy.protocoltests.traits.HttpMalformedResponseBodyDefinition;
import software.amazon.smithy.protocoltests.traits.HttpRequestTestCase;
import software.amazon.smithy.protocoltests.traits.HttpRequestTestsTrait;
import software.amazon.smithy.protocoltests.traits.HttpResponseTestCase;
import software.amazon.smithy.protocoltests.traits.HttpResponseTestsTrait;

/**
 * Generates GoogleTest conformance suites from the official {@code smithy.test#httpRequestTests} /
 * {@code #httpResponseTests} traits (smithy-rs's {@code ClientProtocolTestGenerator} pattern,
 * including its must-shrink exclusion list in {@code protocol-test-exclusions.txt}).
 *
 * <p>Emits {@code tests/request_tests.cc}, {@code tests/response_tests.cc} and a {@code
 * tests/BUILD.bazel} into the generated module.
 */
final class ProtocolTestGenerator {

  private static final String EXCLUSIONS_RESOURCE = "protocol-test-exclusions.txt";

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;
  private final List<OperationShape> operations;
  private final NodeLiteralGenerator literals;

  /** "kind testId" -> reason, for this service only (kind: request|response|error|any). */
  private final Map<String, String> exclusions;

  private final Map<String, String> unusedExclusions;
  private final List<String> excludedHere = new ArrayList<>();

  private final boolean standardTests;
  private final boolean malformedTests;

  ProtocolTestGenerator(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations,
      boolean standardTests,
      boolean malformedTests) {
    this(
        context,
        service,
        protocol,
        operations,
        standardTests,
        malformedTests,
        loadExclusions(service.getId()));
  }

  /** Test seam: exclusions injected instead of loaded from the shared resource. */
  ProtocolTestGenerator(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations,
      boolean standardTests,
      boolean malformedTests,
      Map<String, String> exclusions) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
    this.operations = operations;
    this.standardTests = standardTests;
    this.malformedTests = malformedTests;
    this.literals = new NodeLiteralGenerator(context);
    this.exclusions = exclusions;
    this.unusedExclusions = new LinkedHashMap<>(exclusions);
  }

  void run() {
    if (standardTests) {
      writeRequestTests();
      writeResponseTests();
      writeServerRequestTests();
      writeServerResponseTests();
    }
    if (malformedTests) {
      writeServerMalformedTests();
    }
    // Staleness is only judged for entries whose generation path ran this
    // round: a server-malformed entry is invisible to a malformedTests=false
    // run (and an "any" entry can match in either path), so flagging those
    // would couple the exclusion contract to which paths a task enables.
    List<String> stale =
        unusedExclusions.keySet().stream().filter(key -> pathRan(kindOf(key))).toList();
    if (!stale.isEmpty()) {
      throw new CodegenException(
          "cpp-codegen: protocol-test-exclusions.txt has entries that matched no generated test"
              + " for "
              + service.getId()
              + " (remove them): "
              + stale);
    }
  }

  /** The kind prefix of a "kind testId" exclusion key. */
  private static String kindOf(String key) {
    int space = key.indexOf(' ');
    if (space < 0) {
      throw new CodegenException("cpp-codegen: bad exclusion key (want '<kind> <testId>'): " + key);
    }
    return key.substring(0, space);
  }

  /**
   * Whether this run executed the generation path that consults entries of {@code kind}. The kind
   * vocabulary must stay in step with the {@link #loadExclusions} validation pattern.
   */
  private boolean pathRan(String kind) {
    return switch (kind) {
      case "server-malformed" -> malformedTests;
      case "any" -> standardTests && malformedTests;
      case "request", "response", "error", "server-request", "server-response", "server-error" ->
          standardTests;
      default -> throw new CodegenException("cpp-codegen: unknown exclusion kind: " + kind);
    };
  }

  private String clientType() {
    return CppReservedWords.escape(service.getId().getName()) + "Client";
  }

  /**
   * Whether this test case is skipped here: pinned to another protocol, or named in the exclusion
   * list. Exclusions are consulted — and their entries marked live — even for cross-protocol cases,
   * which never generate in this suite but must not make their entries read as stale; only entries
   * that suppress a test of this suite are listed in the generated header comment.
   */
  private boolean skipped(String kind, ShapeId testProtocol, String testId) {
    boolean thisProtocol = testProtocol.equals(protocol.traitId());
    for (String key : new String[] {kind + " " + testId, "any " + testId}) {
      String reason = exclusions.get(key);
      if (reason != null) {
        unusedExclusions.remove(key);
        if (thisProtocol) {
          excludedHere.add(testId + " (" + kind + ") — " + reason);
        }
        return true;
      }
    }
    return !thisProtocol;
  }

  // ---------------------------------------------------------------------
  // Request tests
  // ---------------------------------------------------------------------

  private void writeRequestTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/request_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              for (OperationShape operation : operations) {
                Optional<HttpRequestTestsTrait> trait =
                    operation.getTrait(HttpRequestTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                for (HttpRequestTestCase testCase : trait.get().getTestCasesFor(AppliesTo.CLIENT)) {
                  if (skipped("request", testCase.getProtocol(), testCase.getId())) {
                    continue;
                  }
                  tests.add(requestTest(operation, testCase));
                }
              }
              writeCommonIncludes(w);
              w.write("// Generated from smithy.test#httpRequestTests (client cases).");
              writeExcludedComment(w);
              writeFixture(w);
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  private String requestTest(OperationShape operation, HttpRequestTestCase testCase) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append("RequestTest, ")
        .append(testCase.getId())
        .append(") {\n");
    // A test host with a path (e.g. "example.com/custom") must prefix the
    // request target, so it flows in as the configured endpoint.
    String endpoint =
        testCase.getHost().map(host -> CppLiterals.stringLiteral("http://" + host)).orElse("");
    t.append("  Fixture fixture = MakeFixture(").append(endpoint).append(");\n");
    t.append("  const ")
        .append(context.cppSymbols().toSymbol(input).getName())
        .append(" input = ")
        .append(literals.expression(input, testCase.getParams()))
        .append(";\n");
    t.append("  (void)fixture.client.")
        .append(CppReservedWords.escape(operation.getId().getName()))
        .append("(input);\n");
    t.append("  const opal::http::HttpRequest& request = fixture.transport->last_request;\n");
    t.append("  EXPECT_EQ(request.method, ")
        .append(CppLiterals.stringLiteral(testCase.getMethod()))
        .append(");\n");
    t.append("  EXPECT_EQ(opal::testing::UriPath(request.target), ")
        .append(CppLiterals.stringLiteral(testCase.getUri()))
        .append(");\n");
    if (!testCase.getQueryParams().isEmpty()) {
      t.append("  EXPECT_TRUE(opal::testing::QueryContains(request.target, ")
          .append(stringVector(testCase.getQueryParams()))
          .append("));\n");
    }
    if (!testCase.getForbidQueryParams().isEmpty()) {
      t.append("  EXPECT_TRUE(opal::testing::QueryForbidsKeys(request.target, ")
          .append(stringVector(testCase.getForbidQueryParams()))
          .append("));\n");
    }
    if (!testCase.getRequireQueryParams().isEmpty()) {
      t.append("  EXPECT_TRUE(opal::testing::QueryRequiresKeys(request.target, ")
          .append(stringVector(testCase.getRequireQueryParams()))
          .append("));\n");
    }
    for (Map.Entry<String, String> header : new TreeMap<>(testCase.getHeaders()).entrySet()) {
      t.append("  EXPECT_EQ(request.headers.Get(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(").value_or(\"<missing>\"), ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    for (String name : testCase.getForbidHeaders()) {
      t.append("  EXPECT_FALSE(request.headers.Has(")
          .append(CppLiterals.stringLiteral(name))
          .append("));\n");
    }
    for (String name : testCase.getRequireHeaders()) {
      t.append("  EXPECT_TRUE(request.headers.Has(")
          .append(CppLiterals.stringLiteral(name))
          .append("));\n");
    }
    testCase
        .getBody()
        .ifPresent(body -> t.append(bodyAssertion(body, testCase.getBodyMediaType())));
    return t.append("}").toString();
  }

  private String bodyAssertion(String body, Optional<String> mediaType) {
    if (body.isEmpty()) {
      return "  EXPECT_TRUE(request.body.empty()) << request.body;\n";
    }
    // When the test omits bodyMediaType, compare under the protocol's own body
    // format: JSON object key order is not significant, so a raw string EXPECT_EQ
    // would spuriously fail (e.g. alloy's RoundTrip response).
    String type = mediaType.orElse(protocol.contentType());
    if (type.equals("application/json") || type.endsWith("+json")) {
      return "  EXPECT_TRUE(opal::testing::JsonBodyEquals("
          + CppLiterals.stringLiteral(body)
          + ", request.body));\n";
    }
    if (type.equals("application/cbor")) {
      return "  EXPECT_TRUE(opal::testing::CborBodyEqualsBase64("
          + CppLiterals.stringLiteral(body)
          + ", request.body));\n";
    }
    return "  EXPECT_EQ(request.body, " + CppLiterals.stringLiteral(body) + ");\n";
  }

  // ---------------------------------------------------------------------
  // Response tests (output cases + error cases)
  // ---------------------------------------------------------------------

  private void writeResponseTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/response_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              for (OperationShape operation : operations) {
                operation
                    .getTrait(HttpResponseTestsTrait.class)
                    .ifPresent(
                        trait -> {
                          for (HttpResponseTestCase testCase :
                              trait.getTestCasesFor(AppliesTo.CLIENT)) {
                            if (skipped("response", testCase.getProtocol(), testCase.getId())) {
                              continue;
                            }
                            tests.add(responseTest(operation, testCase, null));
                          }
                        });
              }
              // Error cases run against the (alphabetically) first operation that
              // declares the error, so each case is generated exactly once.
              Map<String, OperationShape> firstDeclaringOp = new TreeMap<>();
              Map<String, StructureShape> errorByName = new TreeMap<>();
              for (OperationShape operation : operations) {
                for (ShapeId errorId : operation.getErrors(service)) {
                  StructureShape error =
                      context.model().expectShape(errorId).asStructureShape().orElseThrow();
                  String name = context.cppSymbols().toSymbol(error).getName();
                  firstDeclaringOp.putIfAbsent(name, operation);
                  errorByName.putIfAbsent(name, error);
                }
              }
              for (Map.Entry<String, StructureShape> entry : errorByName.entrySet()) {
                StructureShape error = entry.getValue();
                OperationShape operation = firstDeclaringOp.get(entry.getKey());
                Optional<HttpResponseTestsTrait> trait =
                    error.getTrait(HttpResponseTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                for (HttpResponseTestCase testCase :
                    trait.get().getTestCasesFor(AppliesTo.CLIENT)) {
                  if (skipped("error", testCase.getProtocol(), testCase.getId())) {
                    continue;
                  }
                  tests.add(responseTest(operation, testCase, error));
                }
              }
              writeCommonIncludes(w);
              w.write("// Generated from smithy.test#httpResponseTests (client cases),");
              w.write("// including the cases attached to modeled error shapes.");
              writeExcludedComment(w);
              writeFixture(w);
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  /** One response test; {@code error} null for output cases. */
  private String responseTest(
      OperationShape operation, HttpResponseTestCase testCase, StructureShape error) {
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append(error == null ? "ResponseTest, " : "ErrorTest, ")
        .append(testCase.getId())
        .append(") {\n");
    t.append("  Fixture fixture = MakeFixture();\n");
    t.append("  fixture.transport->next_response.status = ")
        .append(testCase.getCode())
        .append(";\n");
    for (Map.Entry<String, String> header : new TreeMap<>(testCase.getHeaders()).entrySet()) {
      t.append("  fixture.transport->next_response.headers.Set(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(", ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    String body = testCase.getBody().orElse("");
    String mediaType = testCase.getBodyMediaType().orElse("");
    if (mediaType.equals("application/cbor")) {
      t.append("  fixture.transport->next_response.body = opal::testing::FromBase64(")
          .append(CppLiterals.stringLiteral(body))
          .append(");\n");
    } else {
      t.append("  fixture.transport->next_response.body = ")
          .append(CppLiterals.stringLiteral(body))
          .append(";\n");
    }
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    t.append("  const auto outcome = fixture.client.")
        .append(CppReservedWords.escape(operation.getId().getName()))
        .append("(")
        .append(context.cppSymbols().toSymbol(input).getName())
        .append("{});\n");
    if (error == null) {
      StructureShape output = ProtocolSupport.outputShape(context, operation);
      t.append("  ASSERT_TRUE(outcome.ok()) << outcome.error().message();\n");
      t.append("  const ")
          .append(context.cppSymbols().toSymbol(output).getName())
          .append(" expected = ")
          .append(literals.expression(output, testCase.getParams()))
          .append(";\n");
      t.append("  EXPECT_EQ(*outcome, expected);\n");
    } else {
      String errorType = context.cppSymbols().toSymbol(error).getName();
      t.append("  ASSERT_FALSE(outcome.ok());\n");
      // code() carries the wire-level shape name (not the C++ type name).
      t.append("  EXPECT_EQ(outcome.error().code(), ")
          .append(CppLiterals.stringLiteral(error.getId().getName()))
          .append(");\n");
      t.append("  const auto* detail = outcome.error().detail<").append(errorType).append(">();\n");
      t.append("  ASSERT_NE(detail, nullptr);\n");
      t.append("  const ")
          .append(errorType)
          .append(" expected = ")
          .append(literals.expression(error, testCase.getParams()))
          .append(";\n");
      t.append("  EXPECT_EQ(*detail, expected);\n");
    }
    return t.append("}").toString();
  }

  // ---------------------------------------------------------------------
  // Shared emission
  // ---------------------------------------------------------------------

  private void writeCommonIncludes(CppWriter w) {
    w.addInclude("<gtest/gtest.h>");
    w.addInclude("<cstdint>");
    w.addInclude("<limits>");
    w.addInclude("<memory>");
    w.addInclude("<optional>");
    w.addInclude("<string>");
    w.addInclude("<utility>");
    w.addInclude("\"" + context.settings().includePrefix() + "/client.h\"");
    w.addInclude("\"smithy/testing/protocol_test.h\"");
  }

  private void writeExcludedComment(CppWriter w) {
    if (excludedHere.isEmpty()) {
      return;
    }
    w.write("//");
    w.write("// Excluded cases (protocol-test-exclusions.txt; the list must only shrink):");
    for (String line : excludedHere) {
      w.writeWithNoFormatting("//   " + line);
    }
    excludedHere.clear();
    w.write("");
  }

  private void writeFixture(CppWriter w) {
    String client = clientType();
    w.write("namespace {");
    w.write("");
    w.openBlock("struct Fixture {");
    w.write("std::shared_ptr<opal::testing::CapturingTransport> transport;");
    w.write("$L client;", client);
    w.closeBlock("};");
    w.write("");
    w.openBlock("Fixture MakeFixture(const std::string& endpoint = \"\") {");
    w.write("auto transport = std::make_shared<opal::testing::CapturingTransport>();");
    w.write("opal::ClientConfig config;");
    w.write("config.retry.max_attempts = 1;  // wire-exact tests: no retries");
    w.write("config.http_client = transport;");
    w.write("config.endpoint = endpoint;");
    w.write("// Create cannot fail when a transport is injected.");
    w.write("auto client = $L::Create(std::move(config));", client);
    w.write("return Fixture{std::move(transport), *std::move(client)};");
    w.closeBlock("}");
    w.write("");
    w.write("}  // namespace");
    w.write("");
  }

  private static String stringVector(List<String> values) {
    StringBuilder out = new StringBuilder("{");
    for (int i = 0; i < values.size(); i++) {
      if (i > 0) {
        out.append(", ");
      }
      out.append(CppLiterals.stringLiteral(values.get(i)));
    }
    return out.append('}').toString();
  }

  // ---------------------------------------------------------------------
  // Server-mode tests: feed wire requests to the generated server.
  // ---------------------------------------------------------------------

  private StructureShape inputOf(OperationShape operation) {
    return ProtocolSupport.inputShape(context, operation);
  }

  private StructureShape outputOf(OperationShape operation) {
    return ProtocolSupport.outputShape(context, operation);
  }

  private String serverType() {
    return CppReservedWords.escape(service.getId().getName()) + "Server";
  }

  private String handlerType() {
    return CppReservedWords.escape(service.getId().getName()) + "Handler";
  }

  private void writeServerCommonIncludes(CppWriter w) {
    writeCommonIncludes(w);
    w.addInclude("\"" + context.settings().includePrefix() + "/server.h\"");
    w.addInclude("<optional>");
  }

  /** RecordingHandler: stores the last parsed input per operation, answers minimally. */
  private void writeRecordingHandler(CppWriter w) {
    for (OperationShape operation : operations) {
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        continue; // no unary answer to minimize; the stub below suffices
      }
      String opName = CppReservedWords.escape(operation.getId().getName());
      w.openBlock(
          "$L Minimal$LOutput() {",
          context.cppSymbols().toSymbol(outputOf(operation)).getName(),
          opName);
      w.writeWithNoFormatting("  return " + literals.minimalExpression(outputOf(operation)) + ";");
      w.closeBlock("}");
      w.write("");
    }
    w.openBlock("class RecordingHandler : public $L {", handlerType());
    w.write("public:").indent();
    for (OperationShape operation : operations) {
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // The unary-shaped conformance harness never upgrades; the stub
        // keeps the interface implemented (the authored stream suite owns
        // the streaming wire, ADR-0023).
        EventStreamCodeGen.writeTestHandlerStub(w, context, operation);
        continue;
      }
      String opName = CppReservedWords.escape(operation.getId().getName());
      String inputType = context.cppSymbols().toSymbol(inputOf(operation)).getName();
      ProtocolSupport.openTestHandlerOverride(
          w, context.cppSymbols().toSymbol(outputOf(operation)).getName(), opName, inputType);
      w.write("last$L = input;", opName);
      w.write("return Minimal$LOutput();", opName);
      w.closeBlock("}");
      w.write("std::optional<$L> last$L;", inputType, opName);
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
  }

  /** The wire request the test case describes. */
  private String wireRequest(
      String method,
      String uri,
      List<String> queryParams,
      Map<String, String> headers,
      Optional<String> body,
      Optional<String> mediaType) {
    StringBuilder t = new StringBuilder();
    t.append("  opal::http::HttpRequest request;\n");
    t.append("  request.method = ").append(CppLiterals.stringLiteral(method)).append(";\n");
    StringBuilder target = new StringBuilder(uri);
    if (!queryParams.isEmpty()) {
      target.append('?').append(String.join("&", queryParams));
    }
    t.append("  request.target = ")
        .append(CppLiterals.stringLiteral(target.toString()))
        .append(";\n");
    for (Map.Entry<String, String> header : new TreeMap<>(headers).entrySet()) {
      t.append("  request.headers.Set(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(", ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    if (body.isPresent() && !body.get().isEmpty()) {
      if (mediaType.orElse(protocol.contentType()).equals("application/cbor")) {
        t.append("  request.body = opal::testing::FromBase64(")
            .append(CppLiterals.stringLiteral(body.get()))
            .append(");\n");
      } else {
        t.append("  request.body = ").append(CppLiterals.stringLiteral(body.get())).append(";\n");
      }
    }
    return t.toString();
  }

  private void writeServerRequestTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/server_request_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              for (OperationShape operation : operations) {
                Optional<HttpRequestTestsTrait> trait =
                    operation.getTrait(HttpRequestTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                for (HttpRequestTestCase testCase : trait.get().getTestCasesFor(AppliesTo.SERVER)) {
                  if (skipped("server-request", testCase.getProtocol(), testCase.getId())) {
                    continue;
                  }
                  tests.add(serverRequestTest(operation, testCase));
                }
              }
              writeServerCommonIncludes(w);
              w.write("// Generated from smithy.test#httpRequestTests (server cases): the wire");
              w.write("// request is routed into the generated server and the parsed input is");
              w.write("// compared against the expected params.");
              writeExcludedComment(w);
              w.write("namespace {");
              w.write("");
              writeRecordingHandler(w);
              w.write("}  // namespace");
              w.write("");
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  private String serverRequestTest(OperationShape operation, HttpRequestTestCase testCase) {
    String opName = CppReservedWords.escape(operation.getId().getName());
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append("ServerRequestTest, ")
        .append(testCase.getId())
        .append(") {\n");
    t.append("  auto handler = std::make_shared<RecordingHandler>();\n");
    t.append("  ").append(serverType()).append(" server(handler);\n");
    t.append(
        wireRequest(
            testCase.getMethod(),
            testCase.getUri(),
            testCase.getQueryParams(),
            testCase.getHeaders(),
            testCase.getBody(),
            testCase.getBodyMediaType()));
    t.append("  const opal::http::HttpResponse response = server.Handler()(request);\n");
    t.append("  ASSERT_TRUE(handler->last")
        .append(opName)
        .append(".has_value()) << response.status << \" \" << response.body;\n");
    t.append("  const ")
        .append(context.cppSymbols().toSymbol(inputOf(operation)).getName())
        .append(" expected = ")
        .append(literals.expression(inputOf(operation), testCase.getParams()))
        .append(";\n");
    t.append("  EXPECT_EQ(*handler->last").append(opName).append(", expected);\n");
    return t.append("}").toString();
  }

  private void writeServerResponseTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/server_response_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              java.util.Set<OperationShape> needsRequest = new java.util.LinkedHashSet<>();
              for (OperationShape operation : operations) {
                operation
                    .getTrait(HttpResponseTestsTrait.class)
                    .ifPresent(
                        trait -> {
                          for (HttpResponseTestCase testCase :
                              trait.getTestCasesFor(AppliesTo.SERVER)) {
                            if (skipped(
                                "server-response", testCase.getProtocol(), testCase.getId())) {
                              continue;
                            }
                            needsRequest.add(operation);
                            tests.add(serverResponseTest(operation, testCase, null));
                          }
                        });
              }
              Map<String, OperationShape> firstDeclaringOp = new TreeMap<>();
              Map<String, StructureShape> errorByName = new TreeMap<>();
              for (OperationShape operation : operations) {
                for (ShapeId errorId : operation.getErrors(service)) {
                  StructureShape error =
                      context.model().expectShape(errorId).asStructureShape().orElseThrow();
                  String name = context.cppSymbols().toSymbol(error).getName();
                  firstDeclaringOp.putIfAbsent(name, operation);
                  errorByName.putIfAbsent(name, error);
                }
              }
              for (Map.Entry<String, StructureShape> entry : errorByName.entrySet()) {
                StructureShape error = entry.getValue();
                OperationShape operation = firstDeclaringOp.get(entry.getKey());
                Optional<HttpResponseTestsTrait> trait =
                    error.getTrait(HttpResponseTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                for (HttpResponseTestCase testCase :
                    trait.get().getTestCasesFor(AppliesTo.SERVER)) {
                  if (skipped("server-error", testCase.getProtocol(), testCase.getId())) {
                    continue;
                  }
                  needsRequest.add(operation);
                  tests.add(serverResponseTest(operation, testCase, error));
                }
              }
              writeServerCommonIncludes(w);
              w.write("// Generated from smithy.test#httpResponseTests (server cases): a stub");
              w.write("// handler returns the expected params and the wire response the server");
              w.write("// produced is compared against the test definition.");
              writeExcludedComment(w);
              w.write("namespace {");
              w.write("");
              writeRecordingHandler(w);
              for (OperationShape operation : needsRequest) {
                writeMinimalRequestHelper(w, operation);
              }
              w.write("}  // namespace");
              w.write("");
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  /** A routable wire request for the operation, captured from the generated client. */
  private void writeMinimalRequestHelper(CppWriter w, OperationShape operation) {
    String opName = CppReservedWords.escape(operation.getId().getName());
    w.openBlock("opal::http::HttpRequest MinimalRequestFor$L() {", opName);
    w.write("auto transport = std::make_shared<opal::testing::CapturingTransport>();");
    w.write("opal::ClientConfig config;");
    w.write("config.retry.max_attempts = 1;  // wire-exact tests: no retries");
    w.write("config.http_client = transport;");
    w.write("auto client = *$L::Create(std::move(config));", clientType());
    w.writeWithNoFormatting(
        "  "
            + context.cppSymbols().toSymbol(inputOf(operation)).getName()
            + " input = "
            + literals.minimalExpression(inputOf(operation))
            + ";");
    for (String override : serverLabelOverrides(operation)) {
      w.write("$L", override);
    }
    w.write("(void)client.$L(input);", opName);
    w.write("return transport->last_request;");
    w.closeBlock("}");
    w.write("");
  }

  /** Non-empty @httpLabel values so the minimal request routes (mirrors the smoke tests). */
  private List<String> serverLabelOverrides(OperationShape operation) {
    List<String> overrides = new ArrayList<>();
    if (operation.getTrait(software.amazon.smithy.model.traits.HttpTrait.class).isEmpty()) {
      return overrides;
    }
    var index = software.amazon.smithy.model.knowledge.HttpBindingIndex.of(context.model());
    for (var binding : index.getRequestBindings(operation).values()) {
      if (binding.getLocation()
          != software.amazon.smithy.model.knowledge.HttpBinding.Location.LABEL) {
        continue;
      }
      var target = context.model().expectShape(binding.getMember().getTarget());
      String field = "input." + context.cppSymbols().toMemberName(binding.getMember());
      switch (target.getType()) {
        case STRING -> overrides.add(field + " = \"smoke\";");
        // Enums: the minimal value already picks a real (non-empty, constraint-
        // valid) member, so no override — forcing "smoke" would fail enum
        // validation on constrained enums.
        default -> {}
      }
    }
    return overrides;
  }

  /** One server response test; {@code error} null for output cases. */
  private String serverResponseTest(
      OperationShape operation, HttpResponseTestCase testCase, StructureShape error) {
    String opName = CppReservedWords.escape(operation.getId().getName());
    String outputType = context.cppSymbols().toSymbol(outputOf(operation)).getName();
    String inputType = context.cppSymbols().toSymbol(inputOf(operation)).getName();
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append(error == null ? "ServerResponseTest, " : "ServerErrorTest, ")
        .append(testCase.getId())
        .append(") {\n");
    t.append("  class Handler final : public RecordingHandler {\n");
    t.append("   public:\n");
    t.append("    opal::Outcome<")
        .append(outputType)
        .append("> ")
        .append(opName)
        .append("(const ")
        .append(inputType)
        .append("& input, ")
        .append(ProtocolSupport.REQUEST_CONTEXT_PARAM)
        .append(") override {\n");
    t.append("      (void)input;\n");
    if (error == null) {
      t.append("      return ")
          .append(literals.expression(outputOf(operation), testCase.getParams()))
          .append(";\n");
    } else {
      t.append("      opal::Error error = opal::Error::Modeled(")
          .append(CppLiterals.stringLiteral(error.getId().getName()))
          .append(", \"\");\n");
      t.append("      error.set_detail(")
          .append(literals.expression(error, testCase.getParams()))
          .append(");\n");
      t.append("      return error;\n");
    }
    t.append("    }\n");
    t.append("  };\n");
    t.append("  ").append(serverType()).append(" server(std::make_shared<Handler>());\n");
    t.append("  const opal::http::HttpResponse response = server.Handler()(MinimalRequestFor")
        .append(opName)
        .append("());\n");
    t.append("  EXPECT_EQ(response.status, ").append(testCase.getCode()).append(");\n");
    for (Map.Entry<String, String> header : new TreeMap<>(testCase.getHeaders()).entrySet()) {
      t.append("  EXPECT_EQ(response.headers.Get(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(").value_or(\"<missing>\"), ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    String body = testCase.getBody().orElse(null);
    if (body != null && !body.isEmpty()) {
      String mediaType = testCase.getBodyMediaType().orElse(protocol.contentType());
      if (mediaType.equals("application/json") || mediaType.endsWith("+json")) {
        t.append("  EXPECT_TRUE(opal::testing::JsonBodyEquals(")
            .append(CppLiterals.stringLiteral(body))
            .append(", response.body));\n");
      } else if (mediaType.equals("application/cbor")) {
        t.append("  EXPECT_TRUE(opal::testing::CborBodyEqualsBase64(")
            .append(CppLiterals.stringLiteral(body))
            .append(", response.body));\n");
      } else {
        t.append("  EXPECT_EQ(response.body, ")
            .append(CppLiterals.stringLiteral(body))
            .append(");\n");
      }
    }
    return t.append("}").toString();
  }

  private void writeServerMalformedTests() {
    context
        .writerDelegator()
        .useFileWriter(
            "tests/server_malformed_tests.cc",
            w -> {
              List<String> tests = new ArrayList<>();
              for (OperationShape operation : operations) {
                Optional<HttpMalformedRequestTestsTrait> trait =
                    operation.getTrait(HttpMalformedRequestTestsTrait.class);
                if (trait.isEmpty()) {
                  continue;
                }
                // getTestCases() expands testParameters into concrete cases.
                for (HttpMalformedRequestTestCase testCase : trait.get().getTestCases()) {
                  if (skipped("server-malformed", testCase.getProtocol(), testCase.getId())) {
                    continue;
                  }
                  tests.add(serverMalformedTest(testCase));
                }
              }
              writeServerCommonIncludes(w);
              w.write("// Generated from smithy.test#httpMalformedRequestTests: each malformed");
              w.write("// wire request is routed into the generated server, which must reject");
              w.write("// it with the expected status/headers/body before the handler runs.");
              writeExcludedComment(w);
              w.write("namespace {");
              w.write("");
              writeRecordingHandler(w);
              w.write("}  // namespace");
              w.write("");
              for (String test : tests) {
                w.writeWithNoFormatting(test);
                w.write("");
              }
            });
  }

  private String serverMalformedTest(HttpMalformedRequestTestCase testCase) {
    var request = testCase.getRequest();
    var response = testCase.getResponse();
    StringBuilder t = new StringBuilder();
    testCase
        .getDocumentation()
        .ifPresent(docs -> docs.lines().forEach(line -> t.append("// ").append(line).append('\n')));
    t.append("TEST(")
        .append(service.getId().getName())
        .append("ServerMalformedTest, ")
        .append(testCase.getId())
        .append(") {\n");
    t.append("  ").append(serverType()).append(" server(std::make_shared<RecordingHandler>());\n");
    t.append(
        wireRequest(
            request.getMethod(),
            request.expectUri(),
            request.getQueryParams(),
            request.getHeaders(),
            request.getBody(),
            request.getBodyMediaType()));
    t.append("  const opal::http::HttpResponse response = server.Handler()(request);\n");
    t.append("  EXPECT_EQ(response.status, ")
        .append(response.getCode())
        .append(") << response.body;\n");
    for (Map.Entry<String, String> header : new TreeMap<>(response.getHeaders()).entrySet()) {
      t.append("  EXPECT_EQ(response.headers.Get(")
          .append(CppLiterals.stringLiteral(header.getKey()))
          .append(").value_or(\"<missing>\"), ")
          .append(CppLiterals.stringLiteral(header.getValue()))
          .append(");\n");
    }
    response.getBody().ifPresent(body -> t.append(malformedBodyAssertion(body)));
    return t.append("}").toString();
  }

  private String malformedBodyAssertion(HttpMalformedResponseBodyDefinition body) {
    if (body.getMessageRegex().isPresent()) {
      return "  EXPECT_TRUE(opal::testing::BodyMessageMatches("
          + CppLiterals.stringLiteral(body.getMessageRegex().get())
          + ", response.body)) << response.body;\n";
    }
    String contents = body.getContents().orElse("");
    if (contents.isEmpty()) {
      return "  EXPECT_TRUE(response.body.empty()) << response.body;\n";
    }
    String mediaType = body.getMediaType();
    if (mediaType.equals("application/json") || mediaType.endsWith("+json")) {
      return "  EXPECT_TRUE(opal::testing::JsonBodyEquals("
          + CppLiterals.stringLiteral(contents)
          + ", response.body)) << response.body;\n";
    }
    if (mediaType.equals("application/cbor")) {
      return "  EXPECT_TRUE(opal::testing::CborBodyEqualsBase64("
          + CppLiterals.stringLiteral(contents)
          + ", response.body));\n";
    }
    return "  EXPECT_EQ(response.body, " + CppLiterals.stringLiteral(contents) + ");\n";
  }

  /** Entries for {@code serviceId} from the shared exclusion resource. */
  private static Map<String, String> loadExclusions(ShapeId serviceId) {
    Map<String, String> exclusions = new LinkedHashMap<>();
    try (InputStream stream =
        ProtocolTestGenerator.class.getResourceAsStream(EXCLUSIONS_RESOURCE)) {
      if (stream == null) {
        return exclusions;
      }
      BufferedReader reader =
          new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8));
      String line;
      while ((line = reader.readLine()) != null) {
        line = line.trim();
        if (line.isEmpty() || line.startsWith("#")) {
          continue;
        }
        String[] parts = line.split("\\s+", 4);
        if (parts.length < 4
            || !parts[1].matches(
                "request|response|error|server-request|server-response|server-error"
                    + "|server-malformed|any")) {
          throw new CodegenException(
              "cpp-codegen: bad exclusion line (want '<service> <request|response|error|any>"
                  + " <testId> <reason>'): "
                  + line);
        }
        if (parts[0].equals(serviceId.toString())) {
          exclusions.put(parts[1] + " " + parts[2], parts[3]);
        }
      }
    } catch (IOException e) {
      throw new CodegenException("cpp-codegen: failed to read " + EXCLUSIONS_RESOURCE, e);
    }
    return exclusions;
  }
}
