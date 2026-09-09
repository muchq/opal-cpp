package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * smithy.protocols#rpcv2Cbor client bindings: POST /service/{Service}/operation/{Operation}, the
 * smithy-protocol header, CBOR request/response/error bodies.
 */
final class Rpcv2CborProtocol implements ProtocolGenerator {

  @Override
  public String name() {
    return "rpcv2Cbor";
  }

  @Override
  public software.amazon.smithy.model.shapes.ShapeId traitId() {
    return software.amazon.smithy.protocol.traits.Rpcv2CborTrait.ID;
  }

  @Override
  public String contentType() {
    return "application/cbor";
  }

  @Override
  public List<String> runtimeDeps() {
    return List.of(":cbor");
  }

  @Override
  public List<String> clientIncludes() {
    return List.of("\"smithy/cbor/cbor.h\"", "\"smithy/core/blob.h\"");
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    ProtocolSupport.writeErrorSupport(
        w,
        "auto doc = opal::cbor::Decode(opal::Blob::FromString(response.body));",
        /* errorTypeHeader= */ "");
  }

  /** rpcv2Cbor error identity travels in the body, never a header; no extra response args. */
  private static final ProtocolSupport.ErrorResponseSpec SPEC =
      new ProtocolSupport.ErrorResponseSpec("CborError", /* errortypeHeader= */ "");

  /** Set up by writeServerHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  @Override
  public List<String> serverIncludes() {
    return List.of("\"smithy/cbor/cbor.h\"", "\"smithy/core/blob.h\"");
  }

  @Override
  public void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    ProtocolSupport.writeErrorBodyHelper(
        w,
        SPEC.errorFn(),
        "application/cbor",
        "opal::cbor::Encode(opal::Document(std::move(body))).ToString()",
        "smithy-protocol",
        "rpc-v2-cbor");
    ProtocolSupport.writeServerErrorToResponse(w, context, service, operations, SPEC);
    // rpcv2Cbor error identity travels in the body, as the fully qualified shape id.
    validation =
        ValidationGenerator.writeWiring(
            w,
            context,
            operations,
            /* alsoEmit= */ false,
            "smithy.framework#ValidationException",
            SPEC);
  }

  @Override
  public void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();

    boolean compressed = ProtocolSupport.gzipCompressed(operation);
    w.openBlock(
        "(void)router_->Add(\"POST\", \"/service/$L/operation/$L\", "
            + "[handler](const opal::http::HttpRequest& $L, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context) -> opal::http::HttpResponse {",
        service.getId().getName(),
        operation.getId().getName(),
        compressed ? "raw_request" : "request");
    if (compressed) {
      w.write("opal::http::HttpRequest request = raw_request;");
      ProtocolSupport.writeRequestDecompression(
          w, operation, "CborError", "SerializationException");
    }
    w.openBlock(
        "if (request.headers.Get(\"smithy-protocol\").value_or(\"\") != \"rpc-v2-cbor\") {");
    w.write(
        "return helpers::CborError(400, \"SerializationException\", "
            + "\"expected smithy-protocol: rpc-v2-cbor\", {});");
    w.closeBlock("}");
    w.write("// Content-Type validation per the rpcv2Cbor spec: a present header must");
    w.write("// carry application/cbor (parameters ignored); 415 otherwise.");
    w.openBlock(
        "if (const auto content_type = request.headers.Get(\"content-type\"); "
            + "content_type.has_value() && "
            + "opal::http::MediaTypeOf(*content_type) != \"application/cbor\") {");
    w.write(
        "return helpers::CborError(415, \"UnsupportedMediaTypeException\", "
            + "\"expected content-type: application/cbor\", {});");
    w.closeBlock("}");
    w.write("$L input{};", inputType);
    if (!ProtocolSupport.noModeledInput(input)) {
      w.write("// An absent body deserializes like an empty CBOR map.");
      w.write("opal::Document body_doc{opal::DocumentMap{}};");
      w.openBlock("if (!request.body.empty()) {");
      w.write("auto decoded = opal::cbor::Decode(opal::Blob::FromString(request.body));");
      w.write(
          "if (!decoded) return helpers::CborError(400, \"SerializationException\", "
              + "decoded.error().message(), {});");
      w.write("body_doc = *std::move(decoded);");
      w.closeBlock("}");
      ProtocolSupport.writeRpcParsedInput(w, context, input, "body_doc", "400", SPEC);
    }
    ProtocolSupport.writeRpcDispatch(w, operation, "handler->", SPEC, validation);
    w.write("opal::http::HttpResponse response;");
    w.write("response.headers.Set(\"smithy-protocol\", \"rpc-v2-cbor\");");
    w.write("response.headers.Set(\"content-type\", \"application/cbor\");");
    w.write(
        "response.body = opal::cbor::Encode(Serialize$L(*outcome)).ToString();",
        SerdeCodeGen.serdeFunctionSuffix(context, output));
    w.write("return response;");
    w.closeBlock("}, $S);", operation.getId().getName());
  }

  @Override
  public boolean supportsEventStreams() {
    return true;
  }

  @Override
  public String eventPayloadEncode(String docExpr) {
    return "opal::cbor::Encode(" + docExpr + ")";
  }

  @Override
  public String eventPayloadDecode(String payloadExpr) {
    return "opal::cbor::Decode(" + payloadExpr + ")";
  }

  @Override
  public void writeStreamingOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    if (!input.members().isEmpty()) {
      // The input carries only the event-stream union — validate() rejected
      // initial members, since the fixed upgrade URI cannot carry them.
      w.write("(void)input;");
    }
    w.write("opal::http::WebSocketDialRequest request;");
    w.write(
        "request.target = path_prefix_ + \"/service/$L/operation/$L\";",
        service.getId().getName(),
        operation.getId().getName());
    w.write("request.headers.Set(\"user-agent\", config_.user_agent);");
    ClientGenerator.writeAuth(w, service);
    EventStreamCodeGen.writeDialAndReturn(w, context, operation);
  }

  @Override
  public void writeStreamServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();
    w.openBlock(
        "(void)stream_router_->Add(\"GET\", \"/service/$L/operation/$L\", "
            + "[handler](const opal::http::HttpRequest& request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, opal::http::WebSocket& socket) {",
        service.getId().getName(),
        operation.getId().getName());
    w.write("(void)request;");
    w.write("// The fixed upgrade URI carries no initial members (ADR-0016): the input");
    w.write("// is empty beyond the stream itself.");
    w.write("$L input{};", inputType);
    EventStreamCodeGen.writeServeAndClose(w, context, operation, "input");
    w.closeBlock("}, $S);", operation.getId().getName());
  }

  @Override
  public void writeStreamSessionRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();
    w.openBlock(
        "(void)stream_router_->AddSession(\"GET\", \"/service/$L/operation/$L\", "
            + "[handler](const opal::http::HttpRequest& request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, std::shared_ptr<opal::http::WebSocket> socket) {",
        service.getId().getName(),
        operation.getId().getName());
    w.write("(void)request;");
    w.write("(void)context;");
    w.write("// The fixed upgrade URI carries no initial members (ADR-0016): the input");
    w.write("// is empty beyond the stream itself.");
    w.write("$L input{};", inputType);
    EventStreamCodeGen.writeLaunchAsync(w, operation, "std::move(input)");
    w.closeBlock("}, $S);", operation.getId().getName());
  }

  @Override
  public void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);

    String in = ProtocolSupport.writeRpcRequestPrelude(w, context, input);
    w.write(
        "request.target = path_prefix_ + \"/service/$L/operation/$L\";",
        service.getId().getName(),
        operation.getId().getName());
    w.write("request.headers.Set(\"smithy-protocol\", \"rpc-v2-cbor\");");
    // Operations with no modeled input (smithy.api#Unit) send no body and no
    // Content-Type, per the rpcv2Cbor spec; empty input structures still send
    // an (empty) CBOR map body.
    if (!ProtocolSupport.noModeledInput(input)) {
      w.write("request.headers.Set(\"content-type\", \"application/cbor\");");
      w.write(
          "request.body = opal::cbor::Encode(Serialize$L($L)).ToString();",
          SerdeCodeGen.serdeFunctionSuffix(context, input),
          in);
    }

    ProtocolSupport.writeRpcSend(w, operation);
    w.write(
        "if (response->status != 200) return $L;",
        ProtocolSupport.errorExpression(context, service, operation));

    if (ProtocolSupport.writeEmptyOutputReturn(w, context, output)) {
      return;
    }
    String outType = context.cppSymbols().toSymbol(output).getName();
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (allOptional) {
      w.write("if (response->body.empty()) return $L{};", outType);
    }
    w.write("auto body_doc = opal::cbor::Decode(opal::Blob::FromString(response->body));");
    w.write("if (!body_doc) return std::move(body_doc).error();");
    w.write("return Deserialize$L(*body_doc);", SerdeCodeGen.serdeFunctionSuffix(context, output));
  }
}
