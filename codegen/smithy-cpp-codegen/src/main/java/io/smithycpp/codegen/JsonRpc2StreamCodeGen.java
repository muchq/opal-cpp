package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * The jsonRpc2 stream wire's generated shapes (ADR-0023): text JSON-RPC 2.0 envelopes on the
 * protocol's shared endpoint. The runtime's {@code jsonrpc_frame}/{@code JsonRpcStreamSocket} owns
 * the mid-stream grammar (notifications, terminal classification); generated code owns what the
 * unary endpoint already owns — building the opening request envelope (client), parsing and
 * validating it (server), and answering with the terminal response envelope through the SAME
 * emitters the unary path uses ({@code JsonRpcError}, {@code ErrorToResponse}, {@code
 * ValidationErrorResponse}) — one error identity, one spelling.
 *
 * <p>Server-side emission order (all in server.cc's helpers namespace, after {@code
 * writeServerHelpers}): the shared helpers here, then per operation the codec pair and the async
 * launch wrapper, then the two shared-endpoint serve drivers the trivial "/" routes call.
 */
final class JsonRpc2StreamCodeGen {

  private JsonRpc2StreamCodeGen() {}

  /**
   * The service-independent helpers: the raw-text message shape, the terminal result envelope, and
   * the opening-envelope parse with the unary endpoint's exact refusal strings.
   */
  static void writeSharedServerHelpers(CppWriter w) {
    w.addInclude("\"opal/eventstream/jsonrpc_stream_socket.h\"");
    w.write("// One JSON-RPC envelope as the raw-text message the stream wire carries");
    w.write("// (ADR-0023): the unary emitters build the envelope, streams reuse their");
    w.write("// bodies verbatim — one error identity, one spelling.");
    w.openBlock(
        "opal::eventstream::Message JsonRpcStreamText(const opal::http::HttpResponse&"
            + " response) {");
    w.write("opal::eventstream::Message message;");
    w.write("message.payload = opal::Blob::FromString(response.body);");
    w.write("return message;");
    w.closeBlock("}");
    w.write("");
    w.write("// The terminal result envelope that ends a clean stream: result stays {}");
    w.write("// while initial-response members remain deferred (ADR-0016/0023).");
    w.openBlock(
        "opal::eventstream::Message BuildJsonRpcTerminalResult(const opal::Document& id) {");
    w.write("opal::DocumentMap envelope;");
    w.write("envelope.emplace(\"jsonrpc\", opal::Document(\"2.0\"));");
    w.write("envelope.emplace(\"result\", opal::Document(opal::DocumentMap{}));");
    w.write("envelope.emplace(\"id\", id);");
    w.write("opal::eventstream::Message message;");
    w.write(
        "message.payload ="
            + " opal::Blob::FromString(opal::json::Encode(opal::Document(std::move(envelope))));");
    w.write("return message;");
    w.closeBlock("}");
    w.write("");
    w.write("// The opening request envelope, parsed and validated (ADR-0023). Refusal");
    w.write("// strings mirror the unary endpoint's exactly — the conformance suite");
    w.write("// compares bodies, so no decoder detail leaks into the wire.");
    w.openBlock("struct JsonRpcOpening {");
    w.write("bool ok = false;");
    w.write("opal::Document id;  // null until the envelope yields one (JSON-RPC 2.0 §5)");
    w.write("std::string method;");
    w.write("opal::Document params{opal::DocumentMap{}};");
    w.write("opal::eventstream::Message refusal;  // when !ok: send, then close");
    w.closeBlock("};");
    w.write("");
    w.openBlock("JsonRpcOpening ParseJsonRpcOpening(const opal::eventstream::Message& message) {");
    w.write("JsonRpcOpening opening;");
    w.openBlock("if (!message.headers.empty()) {");
    w.write("// Only the raw-text wire reaches this route; a framed message means a");
    w.write("// peer speaking the wrong wire entirely.");
    writeOpeningRefusal(w, "-32600", "the opening message must be one JSON-RPC request envelope");
    w.closeBlock("}");
    w.write("auto decoded = opal::json::Decode(message.payload.ToString());");
    w.openBlock("if (!decoded) {");
    writeOpeningRefusal(w, "-32700", "request body is not valid JSON");
    w.closeBlock("}");
    w.openBlock("if (!decoded->is_map()) {");
    writeOpeningRefusal(w, "-32600", "request is not a JSON-RPC 2.0 call");
    w.closeBlock("}");
    w.write(
        "if (const opal::Document* id_doc = decoded->Find(\"id\"); id_doc != nullptr)"
            + " opening.id = *id_doc;");
    w.write("const opal::Document* version = decoded->Find(\"jsonrpc\");");
    w.openBlock(
        "if (version == nullptr || !version->is_string() || version->as_string() != \"2.0\") {");
    writeOpeningRefusal(w, "-32600", "expected jsonrpc: \\\"2.0\\\"");
    w.closeBlock("}");
    w.write("const opal::Document* method = decoded->Find(\"method\");");
    w.openBlock("if (method == nullptr || !method->is_string()) {");
    writeOpeningRefusal(w, "-32600", "expected a string method member");
    w.closeBlock("}");
    w.write("// A call without an id is a notification: nothing to answer, nothing for");
    w.write("// the stream's events to echo — refused, unlike the unary endpoint.");
    w.openBlock("if (opening.id.is_null()) {");
    writeOpeningRefusal(w, "-32600", "the opening call must carry an id");
    w.closeBlock("}");
    w.write("// Absent/null params deserialize like an empty object.");
    w.write("const opal::Document* params = decoded->Find(\"params\");");
    w.write("if (params != nullptr && !params->is_null()) opening.params = *params;");
    w.write("opening.method = method->as_string();");
    w.write("opening.ok = true;");
    w.write("return opening;");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * One opening-envelope refusal: set {@code opening.refusal} to the reserved-code envelope (the
   * unary endpoint's exact SerializationException strings) and return. {@code escapedMessage} is
   * already C++-string-escaped where it needs to be.
   */
  private static void writeOpeningRefusal(CppWriter w, String code, String escapedMessage) {
    w.write(
        "opening.refusal = helpers::JsonRpcStreamText(helpers::JsonRpcError($L,"
            + " \"SerializationException\", \"$L\", {}, opening.id));",
        code,
        escapedMessage);
    w.write("return opening;");
  }

  /**
   * The async route's launch wrapper, jsonRpc2 flavor (ADR-0021/0023): its frame owns the wrapped
   * session, and the stream ends with the terminal response envelope — result on a clean
   * completion, the unary error identity otherwise — AWAITED so the frame (and the stream it owns)
   * outlives the write, then closed.
   */
  static void writeAsyncServeWrapper(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    String op = EventStreamCodeGen.opName(operation);
    String serviceName = CppReservedWords.escape(service.getId().getName());
    String inputType = context.cppSymbols().typeRef(ProtocolSupport.inputShape(context, operation));
    w.write("// The async route's launch wrapper (ADR-0021), on the JSON-RPC wire");
    w.write("// (ADR-0023): the handler speaks envelope messages, the wrapper socket");
    w.write("// translates, and the terminal response envelope — result on a clean");
    w.write("// completion, the unary error identity otherwise — is AWAITED so this");
    w.write("// frame (and the stream it owns) outlives the write. Best-effort, like");
    w.write("// every terminal send: a send the dead session refuses is discarded.");
    w.openBlock(
        "opal::eventstream::Detached Serve$LAsync(std::shared_ptr<types::$LAsyncHandler> handler,"
            + " $L input, std::shared_ptr<opal::http::WebSocket> socket, opal::Document id) {",
        op,
        serviceName,
        inputType);
    w.write(
        "auto wrapped = std::make_shared<opal::eventstream::JsonRpcStreamSocket>(socket, id,"
            + " opal::eventstream::JsonRpcStreamSocket::Role::kServer);");
    w.write(
        "types::$L stream(wrapped, $L, helpers::Decode$LEvent);",
        EventStreamCodeGen.asyncServerStreamAlias(operation),
        EventStreamCodeGen.encoderArgument(
            EventStreamCodeGen.outputInfo(context.model(), operation), operation),
        op);
    w.write("auto outcome = co_await handler->$L(std::move(input), stream);", op);
    w.write("// Built OUTSIDE the co_await expression on purpose: a conditional");
    w.write("// operator inside a co_await full expression miscompiles on GCC (the");
    w.write("// branch temporaries become frame slots and the wrong branch runs).");
    w.write("opal::eventstream::Message terminal =");
    w.write("    outcome.ok() ? BuildJsonRpcTerminalResult(id)");
    w.write(
        "                 : helpers::JsonRpcStreamText(helpers::ErrorToResponse(outcome.error(), id));");
    w.write("(void)co_await opal::eventstream::SendMessage(socket, std::move(terminal));");
    w.write("stream.Close();");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * The two shared-endpoint serve drivers the "/" routes call: the blocking seam's (borrowed
   * socket, blocks through the handler) and the session seam's (a Detached that reads the opening
   * envelope with ReceiveMessage — never parking a handler thread — then fires the operation's
   * launch wrapper). Both parse, refuse with the reserved codes, validate, and dispatch on the
   * opening envelope's method — the unary {@code POST "/"} split of duties, transposed.
   */
  static void writeServeDrivers(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> streamingOperations) {
    ValidationGenerator validation = new ValidationGenerator(context, streamingOperations);
    String serviceName = CppReservedWords.escape(service.getId().getName());

    w.write("// The blocking seam's shared-endpoint driver (ADR-0023).");
    w.openBlock(
        "void ServeJsonRpcStream(types::$LHandler& handler, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, opal::http::WebSocket& socket) {",
        serviceName);
    w.write("auto first = socket.Receive();");
    w.write("// A wire that failed or closed before the opening call is a non-event.");
    w.write("if (!first.ok() || !first->has_value()) return;");
    w.write("const JsonRpcOpening opening = helpers::ParseJsonRpcOpening(**first);");
    w.openBlock("if (!opening.ok) {");
    w.write("(void)socket.Send(opening.refusal);");
    w.write("socket.Close();");
    w.write("return;");
    w.closeBlock("}");
    writeOpeningDispatch(
        w, context, service, protocol, streamingOperations, validation, /* session= */ false);
    w.closeBlock("}");
    w.write("");

    w.write("// The session seam's shared-endpoint driver (ADR-0021/0023): the opening");
    w.write("// envelope is read inside this Detached frame — a client that upgrades");
    w.write("// and never calls costs no parked thread.");
    w.openBlock(
        "opal::eventstream::Detached ServeJsonRpcSession(std::shared_ptr<types::$LAsyncHandler>"
            + " handler, std::shared_ptr<opal::http::WebSocket> socket) {",
        serviceName);
    w.write("auto first = co_await opal::eventstream::ReceiveMessage(socket);");
    w.write("if (!first.ok() || !first->has_value()) co_return;");
    w.write("const JsonRpcOpening opening = helpers::ParseJsonRpcOpening(**first);");
    w.openBlock("if (!opening.ok) {");
    w.write("(void)co_await opal::eventstream::SendMessage(socket, opening.refusal);");
    w.write("socket->Close();");
    w.write("co_return;");
    w.closeBlock("}");
    writeOpeningDispatch(
        w, context, service, protocol, streamingOperations, validation, /* session= */ true);
    w.closeBlock("}");
    w.write("");
  }

  /** The per-operation dispatch ladder shared (in shape) by both drivers. */
  private static void writeOpeningDispatch(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> streamingOperations,
      ValidationGenerator validation,
      boolean session) {
    for (OperationShape operation : streamingOperations) {
      String wireName = operation.getId().getName();
      String op = EventStreamCodeGen.opName(operation);
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      w.openBlock("if (opening.method == $S) {", wireName);
      w.write("$L input{};", context.cppSymbols().typeRef(input));
      if (!ProtocolSupport.noModeledInput(input)) {
        w.write(
            "auto parsed = Deserialize$L(opening.params);",
            SerdeCodeGen.serdeFunctionSuffix(context, input));
        w.openBlock("if (!parsed) {");
        writeRefusalTail(
            w,
            session,
            "helpers::JsonRpcStreamText(helpers::JsonRpcError(-32602, \"SerializationException\","
                + " parsed.error().message(), {}, opening.id))");
        w.closeBlock("}");
        w.write("input = *std::move(parsed);");
      }
      if (validation.validates(operation)) {
        w.write("std::vector<opal::server::ValidationFailure> validation_failures;");
        w.write(
            "helpers::$L(input, \"\", &validation_failures);",
            validation.validatorNameFor(operation));
        w.openBlock("if (!validation_failures.empty()) {");
        writeRefusalTail(
            w,
            session,
            "helpers::JsonRpcStreamText(helpers::ValidationErrorResponse(validation_failures, opening.id))");
        w.closeBlock("}");
      }
      MemberShape streamMember = EventStreamCodeGen.inputStreamMember(context.model(), operation);
      if (streamMember != null) {
        w.write("// The union is the session, never an opening member (ADR-0023).");
        w.write("input.$L.reset();", context.cppSymbols().toMemberName(streamMember));
      }
      if (session) {
        w.write(
            "helpers::Serve$LAsync(handler, std::move(input), std::move(socket), opening.id);", op);
        w.write("co_return;");
      } else {
        w.write(
            "opal::eventstream::JsonRpcStreamSocket wrapped(socket, opening.id,"
                + " opal::eventstream::JsonRpcStreamSocket::Role::kServer);");
        w.write(
            "types::$L stream(wrapped, $L, helpers::Decode$LEvent);",
            EventStreamCodeGen.serverStreamAlias(operation),
            EventStreamCodeGen.encoderArgument(
                EventStreamCodeGen.outputInfo(context.model(), operation), operation),
            op);
        w.write("auto outcome = handler.$L(input, stream, context);", op);
        w.write("// The terminal response rides the raw socket: the wrapper only speaks");
        w.write("// notifications, and the envelope is already text.");
        w.write("(void)socket.Send(outcome.ok() ? helpers::BuildJsonRpcTerminalResult(opening.id)");
        w.write(
            "                                 :"
                + " helpers::JsonRpcStreamText(helpers::ErrorToResponse(outcome.error(), opening.id)));");
        w.write("stream.Close();");
        w.write("return;");
      }
      w.closeBlock("}");
    }
    writeRefusalTail(
        w,
        session,
        "helpers::JsonRpcStreamText(helpers::JsonRpcError(-32601, \"UnknownOperationException\","
            + " \"unknown method: \" + opening.method, {}, opening.id))");
  }

  /** Send one terminal refusal, close, and leave the driver — seam-appropriate forms. */
  private static void writeRefusalTail(CppWriter w, boolean session, String messageExpr) {
    if (session) {
      w.write("(void)co_await opal::eventstream::SendMessage(socket, $L);", messageExpr);
      w.write("socket->Close();");
      w.write("co_return;");
    } else {
      w.write("(void)socket.Send($L);", messageExpr);
      w.write("socket.Close();");
      w.write("return;");
    }
  }

  /**
   * The client streaming method body (ADR-0023): dial the shared endpoint on the raw-text wire,
   * send the opening request envelope — the operation's name as method, the initial-request members
   * as params — then hand the wrapped socket to the typed stream. The terminal response surfaces
   * through the wrapper: result as the stream's clean end, error as the modeled terminal exception.
   */
  static void writeStreamingOperationBody(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      OperationShape operation) {
    w.addInclude("\"opal/eventstream/jsonrpc_stream_socket.h\"");
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    String op = EventStreamCodeGen.opName(operation);
    w.write("opal::http::WebSocketDialRequest request;");
    w.write("// The shared endpoint (ADR-0023): the same target the unary POST uses —");
    w.write("// the operation rides the opening envelope, and every frame is text.");
    w.write("request.target = path_prefix_ + \"/\";");
    w.write("request.raw_text_frames = true;");
    w.write("request.headers.Set(\"user-agent\", config_.user_agent);");
    ClientGenerator.writeAuth(w, service);
    w.write("auto socket = helpers::DialStream(config_, std::move(request));");
    w.write("if (!socket) return std::move(socket).error();");
    w.write("// The opening request envelope: selects the operation and carries the");
    w.write("// initial-request members (the :initial-request seam, realized).");
    if (ProtocolSupport.noModeledInput(input)) {
      w.write("opal::DocumentMap params;");
    } else {
      w.write(
          "opal::DocumentMap params = Serialize$L(input).as_map();",
          SerdeCodeGen.serdeFunctionSuffix(context, input));
      MemberShape streamMember = EventStreamCodeGen.inputStreamMember(context.model(), operation);
      if (streamMember != null) {
        w.write("// The union is the session, never a params member.");
        w.write(
            "params.erase($S);",
            HttpBindingCodeGen.wireName(streamMember, protocol.usesJsonName()));
      }
    }
    w.write("opal::DocumentMap envelope;");
    w.write("envelope.emplace(\"jsonrpc\", opal::Document(\"2.0\"));");
    w.write("envelope.emplace(\"method\", opal::Document($S));", operation.getId().getName());
    w.write("envelope.emplace(\"id\", opal::Document(1));");
    w.write("envelope.emplace(\"params\", opal::Document(std::move(params)));");
    w.write("opal::eventstream::Message opening;");
    w.write(
        "opening.payload ="
            + " opal::Blob::FromString(opal::json::Encode(opal::Document(std::move(envelope))));");
    w.write("auto sent = (*socket)->Send(opening);");
    w.write("if (!sent) return std::move(sent).error();");
    w.write("return $L(", EventStreamCodeGen.clientStreamAlias(operation));
    w.write(
        "    std::make_shared<opal::eventstream::JsonRpcStreamSocket>(*std::move(socket),"
            + " opal::Document(1), opal::eventstream::JsonRpcStreamSocket::Role::kClient),");
    w.write(
        "    $L, helpers::Decode$LEvent);",
        EventStreamCodeGen.encoderArgument(
            EventStreamCodeGen.inputInfo(context.model(), operation), operation),
        op);
  }
}
