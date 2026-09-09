package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * Emits tests/smoke_test.cc: a ready-to-run suite wiring the generated client to the generated
 * server over the loopback transport — every operation round-trips a minimal valid value, and
 * modeled errors map across the wire (the PLAN Phase 4 "generated service smoke tests" item).
 */
final class SmokeTestGenerator {

  private final CppContext context;
  private final ServiceShape service;
  private final List<OperationShape> operations;
  // Streaming operations (ADR-0016) get no generated smoke test: a unary
  // "call, compare one output" round trip has no meaning for an event-stream
  // session, and this harness drives Loopback, which never upgrades. The
  // handler stub still needs their overrides to compile; the chat example's
  // e2e suites are where streams get exercised.
  private final List<OperationShape> unaryOperations;
  private final NodeLiteralGenerator literals;

  SmokeTestGenerator(CppContext context, ServiceShape service, List<OperationShape> operations) {
    this.context = context;
    this.service = service;
    this.operations = operations;
    this.unaryOperations =
        operations.stream()
            .filter(op -> !EventStreamCodeGen.streaming(context.model(), op))
            .toList();
    this.literals = new NodeLiteralGenerator(context);
  }

  private String serviceName() {
    return CppReservedWords.escape(service.getId().getName());
  }

  void run() {
    context.writerDelegator().useFileWriter("tests/smoke_test.cc", this::writeSource);
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

  /**
   * Assignments forcing @httpLabel-bound string/enum members to non-empty values — empty label
   * segments never route (and "/a/" is deliberately routed like "/a").
   */
  private List<String> labelOverrides(OperationShape operation) {
    List<String> overrides = new java.util.ArrayList<>();
    if (operation.getTrait(software.amazon.smithy.model.traits.HttpTrait.class).isEmpty()) {
      return overrides;
    }
    var index = software.amazon.smithy.model.knowledge.HttpBindingIndex.of(context.model());
    for (var binding : index.getRequestBindings(operation).values()) {
      if (binding.getLocation()
          != software.amazon.smithy.model.knowledge.HttpBinding.Location.LABEL) {
        continue;
      }
      var member = binding.getMember();
      var target = context.model().expectShape(member.getTarget());
      String field = "input." + context.cppSymbols().toMemberName(member);
      switch (target.getType()) {
        case STRING -> overrides.add(field + " = \"smoke\";");
        // Enums: the minimal value already picks a real (non-empty, constraint-
        // valid) member, so no override — forcing "smoke" would fail enum
        // validation on constrained enums.
        default -> {
          // Numbers, booleans, timestamps, and enums format to non-empty text
          // already.
        }
      }
    }
    return overrides;
  }

  /** True when the output binds an empty-prefix @httpPrefixHeaders member (matches all). */
  private boolean capturesAllResponseHeaders(OperationShape operation) {
    if (operation.getTrait(software.amazon.smithy.model.traits.HttpTrait.class).isEmpty()) {
      return false;
    }
    var index = software.amazon.smithy.model.knowledge.HttpBindingIndex.of(context.model());
    return index.getResponseBindings(operation).values().stream()
        .anyMatch(
            b ->
                b.getLocation()
                        == software.amazon.smithy.model.knowledge.HttpBinding.Location
                            .PREFIX_HEADERS
                    && b.getLocationName().isEmpty());
  }

  /** "v.Status = 201;" when the output binds @httpResponseCode, else null. */
  private String responseCodeOverride(OperationShape operation) {
    if (operation.getTrait(software.amazon.smithy.model.traits.HttpTrait.class).isEmpty()) {
      return null;
    }
    var index = software.amazon.smithy.model.knowledge.HttpBindingIndex.of(context.model());
    for (var binding : index.getResponseBindings(operation).values()) {
      if (binding.getLocation()
          == software.amazon.smithy.model.knowledge.HttpBinding.Location.RESPONSE_CODE) {
        return "v." + context.cppSymbols().toMemberName(binding.getMember()) + " = 201;";
      }
    }
    return null;
  }

  private void writeSmokeInput(CppWriter w, OperationShape operation) {
    List<String> overrides = labelOverrides(operation);
    String qualifier = overrides.isEmpty() ? "const " : "";
    w.writeWithNoFormatting(
        "  "
            + qualifier
            + typeName(input(operation))
            + " input = "
            + literals.minimalExpression(input(operation))
            + ";");
    if (!overrides.isEmpty()) {
      w.write("// @httpLabel members must be non-empty to route.");
      for (String line : overrides) {
        w.write("$L", line);
      }
    }
  }

  private void writeSource(CppWriter w) {
    w.addInclude("<gtest/gtest.h>");
    w.addInclude("<memory>");
    w.addInclude("<utility>");
    w.addInclude("\"" + context.settings().includePrefix() + "/client.h\"");
    w.addInclude("\"" + context.settings().includePrefix() + "/server.h\"");
    w.addInclude("\"smithy/client/config.h\"");
    w.addInclude("\"smithy/http/loopback.h\"");

    String name = serviceName();
    w.write("// Smoke tests for the generated $L service: the generated client calls the", name);
    w.write("// generated server over the in-memory loopback transport. A passing suite");
    w.write("// pins routing, serde symmetry, required members, and error mapping; swap the");
    w.write("// stub handler for a real one to grow it into your service's test suite.");
    w.write("");
    w.write("namespace {");
    w.write("");
    for (OperationShape operation : unaryOperations) {
      StructureShape out = output(operation);
      w.openBlock(
          "$L Minimal$LOutput() {",
          typeName(out),
          CppReservedWords.escape(operation.getId().getName()));
      String responseCodeOverride = responseCodeOverride(operation);
      if (responseCodeOverride == null) {
        w.writeWithNoFormatting("  return " + literals.minimalExpression(out) + ";");
      } else {
        w.writeWithNoFormatting(
            "  " + typeName(out) + " v = " + literals.minimalExpression(out) + ";");
        w.write("// The @httpResponseCode member drives the wire status; use a real 2xx.");
        w.write("$L", responseCodeOverride);
        w.write("return v;");
      }
      w.closeBlock("}");
      w.write("");
    }
    w.openBlock("class SmokeHandler : public $LHandler {", name);
    w.write("public:").indent();
    for (OperationShape operation : operations) {
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        EventStreamCodeGen.writeTestHandlerStub(w, context, operation);
        continue;
      }
      ProtocolSupport.openTestHandlerOverride(
          w,
          typeName(output(operation)),
          CppReservedWords.escape(operation.getId().getName()),
          typeName(input(operation)));
      w.write("(void)input;");
      w.write("return Minimal$LOutput();", CppReservedWords.escape(operation.getId().getName()));
      w.closeBlock("}");
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
    w.openBlock("$LClient MakeClient(std::shared_ptr<$LHandler> handler) {", name, name);
    w.write("$LServer server(std::move(handler));", name);
    w.write("auto loopback = std::make_shared<opal::http::Loopback>();");
    w.write("(void)loopback->Start(server.Handler());");
    w.write("opal::ClientConfig config;");
    w.write("config.retry.max_attempts = 1;  // wire-exact tests: no retries");
    w.write("config.http_client = loopback;");
    w.write("// Create cannot fail when a transport is injected.");
    w.write("return *$LClient::Create(std::move(config));", name);
    w.closeBlock("}");
    w.write("");
    w.write("}  // namespace");
    w.write("");

    for (OperationShape operation : unaryOperations) {
      String opName = CppReservedWords.escape(operation.getId().getName());
      w.openBlock("TEST($LSmokeTest, $LRoundTrips) {", name, opName);
      w.write("$LClient client = MakeClient(std::make_shared<SmokeHandler>());", name);
      writeSmokeInput(w, operation);
      w.write("const auto outcome = client.$L(input);", opName);
      w.write("ASSERT_TRUE(outcome.ok()) << outcome.error().message();");
      if (capturesAllResponseHeaders(operation)) {
        w.write("// An empty-prefix @httpPrefixHeaders output member captures every");
        w.write("// response header (content-type, ...), so exact equality cannot hold.");
      } else {
        w.write("EXPECT_EQ(*outcome, Minimal$LOutput());", opName);
      }
      w.closeBlock("}");
      w.write("");
    }

    writeErrorMappingTest(w);
  }

  /** One test proving a modeled error (with typed detail) maps across the wire. */
  private void writeErrorMappingTest(CppWriter w) {
    OperationShape errorOp = null;
    StructureShape errorShape = null;
    for (OperationShape operation : unaryOperations) {
      List<ShapeId> errors = operation.getErrors(service);
      if (!errors.isEmpty()) {
        errorOp = operation;
        errorShape = context.model().expectShape(errors.get(0)).asStructureShape().orElseThrow();
        break;
      }
    }
    if (errorOp == null) {
      return;
    }
    String name = serviceName();
    String opName = CppReservedWords.escape(errorOp.getId().getName());
    String errorType = context.cppSymbols().toSymbol(errorShape).getName();
    String wireName = errorShape.getId().getName();

    w.openBlock("TEST($LSmokeTest, ModeledErrorsMapAcrossTheWire) {", name);
    w.openBlock("class FailingHandler final : public SmokeHandler {");
    w.write("public:").indent();
    ProtocolSupport.openTestHandlerOverride(
        w, typeName(output(errorOp)), opName, typeName(input(errorOp)));
    w.write("(void)input;");
    w.write("opal::Error error = opal::Error::Modeled($S, \"smoke\");", wireName);
    w.writeWithNoFormatting("    auto detail = " + literals.minimalExpression(errorShape) + ";");
    // A modeled "message" member would otherwise serialize its minimal value and
    // shadow the generic error message on the wire. (Case-sensitive: only the
    // lowercase wire key feeds opal::Error::message() on the client side.)
    for (var member : errorShape.members()) {
      if (!member.getMemberName().equals("message")) {
        continue;
      }
      if (context.model().expectShape(member.getTarget()).isStringShape()) {
        w.write("detail.$L = \"smoke\";", context.cppSymbols().toMemberName(member));
      }
    }
    w.write("error.set_detail(std::move(detail));");
    w.write("return error;");
    w.closeBlock("}");
    w.dedent();
    w.closeBlock("};");
    w.write("");
    w.write("$LClient client = MakeClient(std::make_shared<FailingHandler>());", name);
    writeSmokeInput(w, errorOp);
    w.write("const auto outcome = client.$L(input);", opName);
    w.write("ASSERT_FALSE(outcome.ok());");
    w.write("EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);");
    w.write("EXPECT_EQ(outcome.error().code(), $S);", wireName);
    w.write("EXPECT_EQ(outcome.error().message(), \"smoke\");");
    w.write("EXPECT_NE(outcome.error().detail<$L>(), nullptr);", errorType);
    w.closeBlock("}");
    w.write("");
  }
}
