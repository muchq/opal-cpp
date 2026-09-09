package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.knowledge.HttpBinding;
import software.amazon.smithy.model.knowledge.HttpBindingIndex;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.HttpTrait;

/**
 * Emits tests/integration_test.cc: the generated client exercises the generated server over both
 * the loopback and a real-socket transport (PLAN Phase 5). Per operation: seeded random-input round
 * trips (client serialize → server deserialize compared member-wise, and server serialize → client
 * deserialize likewise), a "maximal" row with every optional member set, one test per modeled error
 * with a random typed detail, and unknown-response-member tolerance through a body-mutating
 * transport.
 */
final class IntegrationTestGenerator {

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;
  private final List<OperationShape> operations;
  // Streaming operations (ADR-0016) get no generated integration tests: the
  // scripted "one input, one output" round trip is unary-shaped, and this
  // harness drives HTTP transports that never upgrade. The scripted handler
  // still needs their overrides to compile; the chat example's e2e suites
  // are where streams get exercised.
  private final List<OperationShape> unaryOperations;
  private final RandomValueGenerator random;

  IntegrationTestGenerator(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
    this.operations = operations;
    this.unaryOperations =
        operations.stream()
            .filter(op -> !EventStreamCodeGen.streaming(context.model(), op))
            .toList();
    this.random = new RandomValueGenerator(context);
  }

  void run() {
    context.writerDelegator().useFileWriter("tests/integration_test.cc", this::writeSource);
  }

  private String serviceName() {
    return CppReservedWords.escape(service.getId().getName());
  }

  private String opName(OperationShape operation) {
    return CppReservedWords.escape(operation.getId().getName());
  }

  private StructureShape input(OperationShape operation) {
    return ProtocolSupport.inputShape(context, operation);
  }

  private StructureShape output(OperationShape operation) {
    return ProtocolSupport.outputShape(context, operation);
  }

  private String typeName(StructureShape shape) {
    return context.cppSymbols().toSymbol(shape).getName();
  }

  private String randomName(StructureShape shape) {
    return "Random" + SerdeCodeGen.serdeFunctionSuffix(context, shape);
  }

  /** "output.X = 201;" when the output binds @httpResponseCode (random ints aren't statuses). */
  private String responseCodeOverride(OperationShape operation) {
    if (operation.getTrait(HttpTrait.class).isEmpty()) {
      return null;
    }
    var index = HttpBindingIndex.of(context.model());
    for (var binding : index.getResponseBindings(operation).values()) {
      if (binding.getLocation() == HttpBinding.Location.RESPONSE_CODE) {
        return "output." + context.cppSymbols().toMemberName(binding.getMember()) + " = 201;";
      }
    }
    return null;
  }

  /** Unknown-member injection only works on document(-map) response bodies. */
  private boolean unknownFieldEligible(OperationShape operation) {
    if (operation.getTrait(HttpTrait.class).isEmpty()) {
      return true; // rpcv2Cbor: the response body is always a CBOR map.
    }
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    if (http.getCode() == 204) {
      return false;
    }
    var index = HttpBindingIndex.of(context.model());
    return index.getResponseBindings(operation).values().stream()
        .noneMatch(b -> b.getLocation() == HttpBinding.Location.PAYLOAD);
  }

  private void writeSource(CppWriter w) {
    w.addInclude("<gtest/gtest.h>");
    w.addInclude("<array>");
    w.addInclude("<memory>");
    w.addInclude("<optional>");
    w.addInclude("<string>");
    w.addInclude("<utility>");
    w.addInclude("\"" + context.settings().includePrefix() + "/client.h\"");
    w.addInclude("\"" + context.settings().includePrefix() + "/server.h\"");
    w.addInclude("\"opal/client/config.h\"");
    w.addInclude("\"opal/http/loopback.h\"");
    w.addInclude("\"opal/http/socket_transport.h\"");
    w.addInclude("\"opal/testing/protocol_test.h\"");
    if (protocol.contentType().equals("application/cbor")) {
      w.addInclude("\"opal/cbor/cbor.h\"");
      w.addInclude("\"opal/core/blob.h\"");
    } else {
      w.addInclude("\"opal/json/json.h\"");
    }

    String name = serviceName();
    w.write("// Integration tests for the generated $L service (PLAN Phase 5): the", name);
    w.write("// generated client drives the generated server over the loopback transport");
    w.write("// AND a real socket on an ephemeral port. Random inputs are seeded and");
    w.write("// constraint-valid; a failure reproduces deterministically.");
    w.write("");
    w.write("namespace {");
    w.write("");
    RandomValueGenerator.writeRngStruct(w);
    random.writeBuilders(w);
    writeScriptedHandler(w);
    writeFixture(w);
    w.write("");

    for (OperationShape operation : unaryOperations) {
      writeRoundTripTests(w, operation);
    }
    for (OperationShape operation : unaryOperations) {
      writeErrorTests(w, operation);
    }
    for (OperationShape operation : unaryOperations) {
      if (unknownFieldEligible(operation)) {
        writeUnknownFieldTest(w, operation);
      }
    }
    w.write(
        "INSTANTIATE_TEST_SUITE_P(Transports, $LIntegrationTest, "
            + "::testing::Values(TransportKind::kLoopback, TransportKind::kSocket), "
            + "[](const auto& info) { return info.param == TransportKind::kLoopback ? "
            + "\"Loopback\" : \"Socket\"; });",
        name);
    w.write("");
    w.write("}  // namespace");
  }

  /** Handler that records the parsed input and answers with a scripted output or error. */
  private void writeScriptedHandler(CppWriter w) {
    w.openBlock("class ScriptedHandler final : public $LHandler {", serviceName());
    w.write("public:").indent();
    for (OperationShape operation : operations) {
      String op = opName(operation);
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        EventStreamCodeGen.writeTestHandlerStub(w, context, operation);
        continue;
      }
      ProtocolSupport.openTestHandlerOverride(
          w, typeName(output(operation)), op, typeName(input(operation)));
      w.write("last$L = input;", op);
      w.write("if (next$LError.has_value()) return *next$LError;", op, op);
      w.write("return next$LOutput;", op);
      w.closeBlock("}");
      w.write("std::optional<$L> last$L;", typeName(input(operation)), op);
      w.write("$L next$LOutput{};", typeName(output(operation)), op);
      w.write("std::optional<opal::Error> next$LError;", op);
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
  }

  private void writeFixture(CppWriter w) {
    String name = serviceName();
    w.write("enum class TransportKind { kLoopback, kSocket };");
    w.write("");
    w.openBlock("class $LIntegrationTest : public ::testing::TestWithParam<TransportKind> {", name);
    w.write("protected:").indent();
    w.openBlock("void SetUp() override {");
    w.write("handler_ = std::make_shared<ScriptedHandler>();");
    w.write("server_ = std::make_unique<$LServer>(handler_);", name);
    w.write("opal::ClientConfig config;");
    w.write("config.retry.max_attempts = 1;  // wire-exact tests: no retries");
    w.openBlock("if (GetParam() == TransportKind::kLoopback) {");
    w.write("auto loopback = std::make_shared<opal::http::Loopback>();");
    w.write("ASSERT_TRUE(loopback->Start(server_->Handler()).ok());");
    w.write("config.http_client = loopback;");
    w.closeBlock("} else {");
    w.indent();
    w.write("socket_server_ = std::make_unique<opal::http::SocketHttpServer>();");
    w.write("ASSERT_TRUE(socket_server_->Start(server_->Handler()).ok());");
    w.write("config.endpoint = \"http://127.0.0.1:\" + std::to_string(socket_server_->port());");
    w.closeBlock("}");
    w.write("auto client = $LClient::Create(std::move(config));", name);
    w.write("ASSERT_TRUE(client.ok()) << client.error().message();");
    w.write("client_ = std::make_unique<$LClient>(std::move(*client));", name);
    w.closeBlock("}");
    w.write("");
    w.write("void TearDown() override { if (socket_server_ != nullptr) socket_server_->Stop(); }");
    w.write("");
    w.write("std::shared_ptr<ScriptedHandler> handler_;");
    w.write("std::unique_ptr<$LServer> server_;", name);
    w.write("std::unique_ptr<opal::http::SocketHttpServer> socket_server_;");
    w.write("std::unique_ptr<$LClient> client_;", name);
    w.dedent();
    w.closeBlock("};");
  }

  /** The shared round-trip body: send random input, compare both directions. */
  private void writeRoundTripBody(CppWriter w, OperationShape operation) {
    String op = opName(operation);
    w.write("const $L input = $L(rng);", typeName(input(operation)), randomName(input(operation)));
    w.write("$L output = $L(rng);", typeName(output(operation)), randomName(output(operation)));
    String responseCodeOverride = responseCodeOverride(operation);
    if (responseCodeOverride != null) {
      w.write("// The @httpResponseCode member drives the wire status; use a real 2xx.");
      w.write("$L", responseCodeOverride);
    }
    w.write("handler_->next$LOutput = output;", op);
    w.write("const auto outcome = client_->$L(input);", op);
    w.write("ASSERT_TRUE(outcome.ok()) << outcome.error().message();");
    w.write("ASSERT_TRUE(handler_->last$L.has_value());", op);
    w.write("EXPECT_EQ(*handler_->last$L, input);", op);
    w.write("EXPECT_EQ(*outcome, output);");
  }

  private void writeRoundTripTests(CppWriter w, OperationShape operation) {
    String name = serviceName();
    String op = opName(operation);
    w.openBlock("TEST_P($LIntegrationTest, $LRandomRoundTrips) {", name, op);
    w.write("Rng rng{std::mt19937{20260707U}, /*fill_all=*/false};");
    w.openBlock("for (int iteration = 0; iteration < 8; ++iteration) {");
    writeRoundTripBody(w, operation);
    w.closeBlock("}");
    w.closeBlock("}");
    w.write("");
    w.openBlock("TEST_P($LIntegrationTest, $LMaximalRoundTrips) {", name, op);
    w.write("Rng rng{std::mt19937{7U}, /*fill_all=*/true};");
    writeRoundTripBody(w, operation);
    w.closeBlock("}");
    w.write("");
  }

  /** One test per modeled error: a random typed detail must survive the wire. */
  private void writeErrorTests(CppWriter w, OperationShape operation) {
    String name = serviceName();
    String op = opName(operation);
    for (ShapeId errorId : operation.getErrors(service)) {
      StructureShape error = context.model().expectShape(errorId).asStructureShape().orElseThrow();
      String errorType = context.cppSymbols().toSymbol(error).getName();
      String wireName = error.getId().getName();
      w.openBlock("TEST_P($LIntegrationTest, $L$LMapsAcrossTheWire) {", name, op, errorType);
      w.write("Rng rng{std::mt19937{42U}, /*fill_all=*/true};");
      w.write(
          "const $L detail = Random$L(rng);",
          errorType,
          SerdeCodeGen.serdeFunctionSuffix(context, error));
      w.write("opal::Error error = opal::Error::Modeled($S, \"integration\");", wireName);
      w.write("error.set_detail(detail);");
      w.write("handler_->next$LError = error;", op);
      writeMinimalCall(w, operation);
      w.write("ASSERT_FALSE(outcome.ok());");
      w.write("EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);");
      w.write("EXPECT_EQ(outcome.error().code(), $S);", wireName);
      w.write("ASSERT_NE(outcome.error().detail<$L>(), nullptr);", errorType);
      w.write("EXPECT_EQ(*outcome.error().detail<$L>(), detail);", errorType);
      w.closeBlock("}");
      w.write("");
    }
  }

  /** Calls the operation with a routable random input, leaving `outcome` in scope. */
  private void writeMinimalCall(CppWriter w, OperationShape operation) {
    w.write("const $L input = $L(rng);", typeName(input(operation)), randomName(input(operation)));
    w.write("const auto outcome = client_->$L(input);", opName(operation));
  }

  /** Servers may add members old clients don't know; clients must ignore them. */
  private void writeUnknownFieldTest(CppWriter w, OperationShape operation) {
    String name = serviceName();
    String op = opName(operation);
    boolean cbor = protocol.contentType().equals("application/cbor");
    w.openBlock("TEST($LIntegrationUnknownMembers, $LToleratesUnknownResponseMembers) {", name, op);
    w.write("auto handler = std::make_shared<ScriptedHandler>();");
    w.write("$LServer server(handler);", name);
    w.write("auto loopback = std::make_shared<opal::http::Loopback>();");
    w.write("ASSERT_TRUE(loopback->Start(server.Handler()).ok());");
    w.openBlock("auto inject = [](opal::http::HttpResponse& response) {");
    if (cbor) {
      w.write("auto doc = opal::cbor::Decode(opal::Blob::FromString(response.body));");
    } else {
      w.write("auto doc = opal::json::Decode(response.body);");
    }
    w.write("if (!doc.ok() || !doc->is_map()) return;");
    w.write("auto map = doc->as_map();");
    w.write("map.insert_or_assign(\"smithy_cpp_unknown_member\", opal::Document(42));");
    if (cbor) {
      w.write("response.body = opal::cbor::Encode(opal::Document(std::move(map))).ToString();");
    } else {
      w.write("response.body = opal::json::Encode(opal::Document(std::move(map)));");
    }
    w.closeBlock("};");
    w.write(
        "auto transport = std::make_shared<opal::testing::MutatingTransport>(loopback, "
            + "inject);");
    w.write("opal::ClientConfig config;");
    w.write("config.retry.max_attempts = 1;  // wire-exact tests: no retries");
    w.write("config.http_client = transport;");
    w.write("auto client = *$LClient::Create(std::move(config));", name);
    w.write("Rng rng{std::mt19937{99U}, /*fill_all=*/true};");
    w.write("const $L input = $L(rng);", typeName(input(operation)), randomName(input(operation)));
    w.write("$L output = $L(rng);", typeName(output(operation)), randomName(output(operation)));
    String responseCodeOverride = responseCodeOverride(operation);
    if (responseCodeOverride != null) {
      w.write("$L", responseCodeOverride);
    }
    w.write("handler->next$LOutput = output;", op);
    w.write("const auto outcome = client.$L(input);", op);
    w.write("ASSERT_TRUE(outcome.ok()) << outcome.error().message();");
    w.write("EXPECT_EQ(*outcome, output);");
    w.closeBlock("}");
    w.write("");
  }
}
