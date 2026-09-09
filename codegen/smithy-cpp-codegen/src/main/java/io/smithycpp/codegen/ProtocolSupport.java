package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.IdempotencyTokenTrait;
import software.amazon.smithy.model.traits.RetryableTrait;

/** Emission helpers shared by the concrete protocol generators. */
final class ProtocolSupport {

  /** The handler methods' second parameter (ADR-0010), spelled once. */
  static final String REQUEST_CONTEXT_PARAM = "const opal::server::RequestContext&";

  /**
   * Opens a generated test-stub handler override — the one signature every test generator's handler
   * subclass emits, so the stubs cannot drift from the interface ServerGenerator writes.
   */
  static void openTestHandlerOverride(
      CppWriter w, String outputType, String opName, String inputType) {
    w.openBlock(
        "opal::Outcome<$L> $L(const $L& input, $L) override {",
        outputType,
        opName,
        inputType,
        REQUEST_CONTEXT_PARAM);
  }

  private ProtocolSupport() {}

  /**
   * Opens the file-local helpers namespace (issue #71): a named namespace nested in the anonymous
   * one, so helpers keep internal linkage while every reference to them is spelled {@code
   * helpers::X} — a model shape's type name can never capture a helper call through C++ name
   * lookup, and no model name is reserved. The mirror-image direction is covered by the {@code
   * types} alias every generated source carries ({@link CppWriter}): code inside the namespace
   * references model types as {@code types::X}, which a sibling helper's name can never shadow.
   */
  static void openHelpersNamespace(CppWriter w) {
    w.write("namespace {");
    w.write("namespace helpers {");
    w.write("");
  }

  /** Closes {@link #openHelpersNamespace}'s two scopes. */
  static void closeHelpersNamespace(CppWriter w) {
    w.write("}  // namespace helpers");
    w.write("}  // namespace");
    w.write("");
  }

  /** Emits SanitizeErrorCode: strips URI qualifiers and namespaces off wire error codes. */
  static void writeSanitizeErrorCode(CppWriter w) {
    w.write("// The error shape name arrives namespaced (\"ns#Shape\") and possibly");
    w.write("// URI-qualified; modeled error codes keep only the shape name.");
    w.openBlock("std::string SanitizeErrorCode(std::string_view raw) {");
    w.write(
        "if (const auto colon = raw.find(':'); colon != std::string_view::npos) "
            + "raw = raw.substr(0, colon);");
    w.write(
        "if (const auto hash = raw.find('#'); hash != std::string_view::npos) "
            + "raw = raw.substr(hash + 1);");
    w.write("return std::string(raw);");
    w.closeBlock("}");
    w.write("");
  }

  /** Emits the ParsedError struct the Make&lt;Error&gt;Error deserializers consume. */
  static void writeParsedErrorStruct(CppWriter w) {
    w.openBlock("struct ParsedError {");
    w.write("int status = 0;");
    w.write("std::string code = \"UnknownError\";");
    w.write("std::string message;");
    w.write("opal::Document doc;");
    w.closeBlock("};");
    w.write("");
  }

  /** Emits GenericError: the fallback opal::Error for unrecognized wire errors. */
  static void writeGenericError(CppWriter w) {
    w.openBlock("opal::Error GenericError(ParsedError parsed) {");
    w.write("const bool retryable = parsed.status >= 500;");
    w.write(
        "if (parsed.code == \"UnknownError\") return opal::Error(opal::ErrorKind::kUnknown, "
            + "std::move(parsed.code), std::move(parsed.message), retryable);");
    w.write(
        "return opal::Error::Modeled(std::move(parsed.code), std::move(parsed.message), "
            + "retryable);");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * Emits the protocol's shared error-parsing helpers (SanitizeErrorCode, ParsedError, ParseError,
   * GenericError); only the wire decode and the code-carrying header differ per protocol. Protocols
   * whose error identity is not header/body-top-level shaped (jsonRpc2) compose the pieces
   * themselves and write their own ParseError.
   */
  static void writeErrorSupport(CppWriter w, String decodeStatement, String errorTypeHeader) {
    writeSanitizeErrorCode(w);
    writeParsedErrorStruct(w);
    w.write("// [[maybe_unused]]: only unary response paths parse wire errors; a");
    w.write("// service whose operations all stream never calls this.");
    w.openBlock(
        "[[maybe_unused]] ParsedError ParseError(const opal::http::HttpResponse& response) {");
    w.write("ParsedError parsed;");
    w.write("parsed.status = response.status;");
    w.write("parsed.message = \"HTTP \" + std::to_string(response.status);");
    w.write(decodeStatement);
    w.write("if (doc.ok()) parsed.doc = *std::move(doc);");
    if (!errorTypeHeader.isEmpty()) {
      w.write("const auto type_header = response.headers.Get($S);", errorTypeHeader);
      w.write(
          "if (type_header.has_value()) parsed.code = helpers::SanitizeErrorCode(*type_header);");
    }
    w.openBlock("if (parsed.doc.is_map()) {");
    w.write("const opal::Document* type = parsed.doc.Find(\"__type\");");
    w.write("if (type == nullptr) type = parsed.doc.Find(\"code\");");
    w.write(
        "if (parsed.code == \"UnknownError\" && type != nullptr && type->is_string()) "
            + "parsed.code = helpers::SanitizeErrorCode(type->as_string());");
    w.write("const opal::Document* text = parsed.doc.Find(\"message\");");
    w.write("if (text != nullptr && text->is_string()) parsed.message = text->as_string();");
    w.closeBlock("}");
    w.write("return parsed;");
    w.closeBlock("}");
    w.write("");
    writeGenericError(w);
  }

  /** Text-to-number helpers used for header/label/query bindings. */
  static void writeNumericParseHelpers(CppWriter w) {
    w.addInclude("<algorithm>");
    w.addInclude("<charconv>");
    w.addInclude("<cmath>");
    w.addInclude("<cstdlib>");
    w.write("// Strict text parsing for label/query/header bindings ([[maybe_unused]]:");
    w.write("// emitted for every service; not every service binds numeric values).");
    w.write("// Trailing text, floats-for-ints, and out-of-range values are rejected");
    w.write("// (the malformed-request suites pin this).");
    w.openBlock(
        "[[maybe_unused]] opal::Outcome<std::int64_t> ParseInt64Text(const std::string& text, "
            + "std::int64_t min_value, std::int64_t max_value) {");
    w.write("std::int64_t value = 0;");
    w.write("const char* first = text.data();");
    w.write("const char* last = first + text.size();");
    w.write("const auto result = std::from_chars(first, last, value, 10);");
    w.write(
        "if (text.empty() || result.ec != std::errc() || result.ptr != last || "
            + "value < min_value || value > max_value) {");
    w.indent();
    w.write("return opal::Error::Serialization(\"invalid integer: \" + text);");
    w.dedent();
    w.write("}");
    w.write("return value;");
    w.closeBlock("}");
    w.write("");
    // Floating-point std::from_chars is missing on libc++ (Apple), so doubles
    // pair a strict character-set check (rejects hex, inf/nan spellings, and
    // leading '+'/whitespace strtod would accept) with a fully-consuming strtod.
    w.openBlock(
        "[[maybe_unused]] opal::Outcome<double> ParseDoubleText(const std::string& text) {");
    w.write("if (text == \"NaN\") return std::numeric_limits<double>::quiet_NaN();");
    w.write("if (text == \"Infinity\") return std::numeric_limits<double>::infinity();");
    w.write("if (text == \"-Infinity\") return -std::numeric_limits<double>::infinity();");
    w.openBlock("const auto valid_char = [](char c) {");
    w.write(
        "return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || "
            + "c == '-';");
    w.closeBlock("};");
    w.openBlock(
        "if (text.empty() || text.front() == '+' || "
            + "!std::all_of(text.begin(), text.end(), valid_char)) {");
    w.write("return opal::Error::Serialization(\"invalid number: \" + text);");
    w.closeBlock("}");
    w.write("char* parse_end = nullptr;");
    w.write("const double value = std::strtod(text.c_str(), &parse_end);");
    w.openBlock("if (parse_end != text.c_str() + text.size() || !std::isfinite(value)) {");
    w.write("return opal::Error::Serialization(\"invalid number: \" + text);");
    w.closeBlock("}");
    w.write("return value;");
    w.closeBlock("}");
    w.write("");
  }

  /** min/max arguments for ParseInt64Text per integer shape type. */
  static String int64Bounds(software.amazon.smithy.model.shapes.ShapeType type) {
    return switch (type) {
      case BYTE -> "-128, 127";
      case SHORT -> "-32768, 32767";
      case INTEGER, INT_ENUM -> "-2147483648LL, 2147483647LL";
      default ->
          "std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()";
    };
  }

  /**
   * The service closure's operations, sorted by id — the one spelling of "this service's
   * operations" (ClientGenerator and DirectedCppCodegen's BUILD emission both derive from it, so
   * their closure semantics cannot drift).
   */
  static List<OperationShape> containedOperations(
      software.amazon.smithy.model.Model model, ServiceShape service) {
    List<OperationShape> operations =
        new java.util.ArrayList<>(
            software.amazon.smithy.model.knowledge.TopDownIndex.of(model)
                .getContainedOperations(service));
    operations.sort(java.util.Comparator.comparing(OperationShape::getId));
    return operations;
  }

  /** Whether @requestCompression asks for gzip on this operation. */
  static boolean gzipCompressed(OperationShape operation) {
    return operation
        .getTrait(software.amazon.smithy.model.traits.RequestCompressionTrait.class)
        .map(t -> t.getEncodings().contains("gzip"))
        .orElse(false);
  }

  /**
   * Whether {@code input} is smithy.api#Unit — directly, or via the synthetic dedicated input shape
   * derived from it (OriginalShapeIdTrait). Operations without modeled input send no body/params
   * and no content type.
   */
  static boolean noModeledInput(StructureShape input) {
    return input.getId().toString().equals("smithy.api#Unit")
        || input
            .getTrait(software.amazon.smithy.model.traits.synthetic.OriginalShapeIdTrait.class)
            .map(t -> t.getOriginalId().toString().equals("smithy.api#Unit"))
            .orElse(false);
  }

  /**
   * Client-side @requestCompression: gzip the request body once it reaches the configured minimum
   * size, appending to any member-bound Content-Encoding header. Emitted after the body and every
   * header binding; Send() computes content-length afterwards.
   */
  static void writeRequestCompression(
      CppWriter w, software.amazon.smithy.model.shapes.OperationShape operation) {
    if (!gzipCompressed(operation)) {
      return;
    }
    w.addInclude("\"smithy/compression/gzip.h\"");
    w.addInclude("<cstddef>");
    w.write("// @requestCompression(gzip): applied last, appended to Content-Encoding.");
    w.openBlock(
        "if (request.body.size() >= "
            + "static_cast<std::size_t>(config_.request_min_compression_size_bytes)) {");
    w.write("auto compressed = opal::GzipCompress(request.body);");
    w.write("if (!compressed) return std::move(compressed).error();");
    w.write("request.body = *std::move(compressed);");
    w.write("const auto existing_encoding = request.headers.Get(\"content-encoding\");");
    w.write(
        "request.headers.Set(\"content-encoding\", existing_encoding.has_value() && "
            + "!existing_encoding->empty() ? *existing_encoding + \", gzip\" : \"gzip\");");
    w.closeBlock("}");
  }

  /**
   * Server-side inverse: transparently gunzip request bodies for @requestCompression operations
   * when the (final) Content-Encoding is gzip. Emitted at the top of the route lambda.
   */
  static void writeRequestDecompression(
      CppWriter w, OperationShape operation, String errorFn, String errorCode) {
    if (!gzipCompressed(operation)) {
      return;
    }
    writeGzipRequestDecode(w, errorFn, "400", errorCode, "");
  }

  /**
   * The gzip request-decode block itself, for callers with their own gating and error shape
   * (jsonRpc2's shared endpoint uses the reserved -32700 code and threads the envelope id).
   */
  static void writeGzipRequestDecode(
      CppWriter w, String errorFn, String status, String errorCode, String extraArgs) {
    w.addInclude("\"smithy/compression/gzip.h\"");
    w.write("// @requestCompression(gzip): decode before parsing.");
    w.openBlock(
        "if (const auto request_encoding = request.headers.Get(\"content-encoding\"); "
            + "request_encoding.has_value() && (*request_encoding == \"gzip\" || "
            + "request_encoding->ends_with(\", gzip\"))) {");
    w.write("auto decompressed = opal::GzipDecompress(request.body);");
    w.openBlock("if (!decompressed) {");
    w.write(
        "return helpers::$L($L, $S, \"invalid gzip request body\", {}$L);",
        errorFn,
        status,
        errorCode,
        extraArgs);
    w.closeBlock("}");
    w.write("request.body = *std::move(decompressed);");
    w.closeBlock("}");
  }

  /**
   * Emits {@code <name>(status, code, message, body)} — the protocol's error-body helper that
   * ErrorToResponse and route-level failures call: a non-empty {@code code} lands in the body's
   * __type, a non-empty {@code message} in message, and the body is rendered by {@code encodeExpr}.
   * JsonRpcError stays protocol-specific (envelope nesting, id echo, message-fallback).
   */
  static void writeErrorBodyHelper(
      CppWriter w, String name, String contentType, String encodeExpr) {
    writeErrorBodyHelper(w, name, contentType, encodeExpr, "", "");
  }

  /** Overload with one extra fixed response header (rpcv2Cbor's smithy-protocol). */
  static void writeErrorBodyHelper(
      CppWriter w,
      String name,
      String contentType,
      String encodeExpr,
      String extraHeaderName,
      String extraHeaderValue) {
    w.openBlock(
        "opal::http::HttpResponse $L(int status, const std::string& code, "
            + "const std::string& message, opal::DocumentMap body) {",
        name);
    w.write("if (!code.empty()) body.insert_or_assign(\"__type\", opal::Document(code));");
    w.write("if (!message.empty()) body.insert_or_assign(\"message\", opal::Document(message));");
    w.write("opal::http::HttpResponse response;");
    w.write("response.status = status;");
    if (!extraHeaderName.isEmpty()) {
      w.write("response.headers.Set($S, $S);", extraHeaderName, extraHeaderValue);
    }
    w.write("response.headers.Set(\"content-type\", $S);", contentType);
    w.write("response.body = $L;", encodeExpr);
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * The shared head of every RPC operation body: @idempotencyToken prep and the POST request
   * skeleton (target, headers, and body are the protocol's wire format and stay with the caller).
   * Returns the input variable name.
   */
  static String writeRpcRequestPrelude(CppWriter w, CppContext context, StructureShape input) {
    String in =
        prepareIdempotencyTokens(w, context, input, context.cppSymbols().toSymbol(input).getName());
    w.write("opal::http::HttpRequest request;");
    w.write("request.method = \"POST\";");
    return in;
  }

  /** Compression + Send + transport-error check: every RPC request phase ends here. */
  static void writeRpcSend(CppWriter w, OperationShape operation) {
    writeRequestCompression(w, operation);
    w.write("auto response = Send(std::move(request));");
    w.write("if (!response) return std::move(response).error();");
  }

  /**
   * Emits the early return for operations whose output has no members; returns whether it did (the
   * caller stops emitting response handling).
   */
  static boolean writeEmptyOutputReturn(CppWriter w, CppContext context, StructureShape output) {
    if (!output.members().isEmpty()) {
      return false;
    }
    w.write("return $L{};", context.cppSymbols().toSymbol(output).getName());
    return true;
  }

  /**
   * Deserializes an RPC server's decoded input document into {@code input}, reporting parse
   * failures through the protocol's error emitter ({@code parseErrorStatus} is the protocol's
   * serialization-failure code: rpcv2Cbor's 400, jsonRpc2's -32602). The caller owns the
   * protocol-specific decode framing before this and skips it entirely for no-input operations.
   */
  static void writeRpcParsedInput(
      CppWriter w,
      CppContext context,
      StructureShape input,
      String deserializeFrom,
      String parseErrorStatus,
      ErrorResponseSpec spec) {
    w.write(
        "auto parsed = Deserialize$L($L);",
        SerdeCodeGen.serdeFunctionSuffix(context, input),
        deserializeFrom);
    w.write(
        "if (!parsed) return helpers::$L($L, \"SerializationException\", parsed.error().message(), {}$L);",
        spec.errorFn(),
        parseErrorStatus,
        spec.extraArgs());
    w.write("input = *std::move(parsed);");
  }

  /**
   * The validate→invoke tail every RPC server shares: run the operation's validators over {@code
   * input}, call the handler, and map handler errors through ErrorToResponse. Response
   * serialization stays with the caller.
   */
  static void writeRpcDispatch(
      CppWriter w,
      OperationShape operation,
      String handlerAccess,
      ErrorResponseSpec spec,
      ValidationGenerator validation) {
    validation.writeRouteGuard(w, operation, spec.extraArgs());
    w.write(
        "auto outcome = $L$L(input, context);",
        handlerAccess,
        CppReservedWords.escape(operation.getId().getName()));
    w.write("if (!outcome) return helpers::ErrorToResponse(outcome.error()$L);", spec.extraArgs());
  }

  /** HTTP status for a modeled error shape: @httpError, else @error class default. */
  static int errorStatus(StructureShape shape) {
    var httpError = shape.getTrait(software.amazon.smithy.model.traits.HttpErrorTrait.class);
    if (httpError.isPresent()) {
      return httpError.get().getCode();
    }
    var error = shape.expectTrait(software.amazon.smithy.model.traits.ErrorTrait.class);
    return error.isClientError() ? 400 : 500;
  }

  /**
   * A protocol's error-response identity, shared by ErrorToResponse and the validation wiring: the
   * error-body function name, the error-identity response header ("" when identity travels in the
   * body), and the extra parameter/argument text threaded through every response helper (jsonRpc2's
   * envelope id; "" elsewhere).
   */
  record ErrorResponseSpec(
      String errorFn, String errortypeHeader, String extraParams, String extraArgs) {
    ErrorResponseSpec(String errorFn, String errortypeHeader) {
      this(errorFn, errortypeHeader, "", "");
    }
  }

  /**
   * Emits the server's ErrorToResponse over {@code spec.errorFn()(status, code, message, body)}:
   * modeled errors get their @httpError status and serialized detail; validation/serialization
   * failures map to 400; anything else is a non-leaking 500.
   */
  static void writeServerErrorToResponse(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      List<OperationShape> operations,
      ErrorResponseSpec spec) {
    writeServerErrorToResponse(
        w,
        context,
        service,
        operations,
        spec.errorFn(),
        spec.errortypeHeader(),
        spec.extraParams(),
        spec.extraArgs());
  }

  /**
   * Positional variant behind the {@link ErrorResponseSpec} entry point: {@code extraParams} is
   * appended to ErrorToResponse's signature (e.g. ", const opal::Document& id") and {@code
   * extraArgs} to every {@code errorBodyFn} call (e.g. ", id"). jsonRpc2 uses this to echo the
   * request id into error envelopes.
   */
  private static void writeServerErrorToResponse(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      List<OperationShape> operations,
      String errorBodyFn,
      String errortypeHeader,
      String extraParams,
      String extraArgs) {
    // Every use below is a call, and helper calls carry one qualified spelling.
    String errorCall = "helpers::" + errorBodyFn;
    Map<String, StructureShape> errorShapes = new TreeMap<>();
    for (OperationShape operation : operations) {
      for (ShapeId errorId : operation.getErrors(service)) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        errorShapes.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
    }
    w.write("// [[maybe_unused]]: only unary routes map handler errors here; a service");
    w.write("// whose operations all stream reports errors on the stream instead.");
    w.openBlock(
        "[[maybe_unused]] opal::http::HttpResponse ErrorToResponse(const opal::Error& "
            + "error$L) {",
        extraParams);
    if (!errortypeHeader.isEmpty()) {
      w.write("std::vector<std::pair<std::string, std::string>> header_values;");
      w.write("(void)header_values;");
    }
    w.openBlock("if (error.kind() == opal::ErrorKind::kModeled) {");
    for (StructureShape shape : errorShapes.values()) {
      String type = context.cppSymbols().toSymbol(shape).getName();
      w.openBlock("if (error.code() == $S) {", shape.getId().getName());
      w.write("opal::DocumentMap body;");
      w.openBlock("if (const auto* detail = error.detail<types::$L>()) {", type);
      w.write(
          "body = Serialize$L(*detail).as_map();",
          SerdeCodeGen.serdeFunctionSuffix(context, shape));
      w.closeBlock("}");
      w.write("// The typed detail's own message member wins over the generic one.");
      w.write(
          "const bool has_message = body.count(\"message\") != 0 || "
              + "body.count(\"Message\") != 0;");
      w.openBlock("if (!has_message && !error.message().empty()) {");
      w.write("body.emplace(\"message\", opal::Document(error.message()));");
      w.closeBlock("}");
      if (!errortypeHeader.isEmpty()) {
        // restJson1: @httpHeader-bound error members travel as headers, and the
        // error shape name in X-Amzn-Errortype rather than the body.
        var index = software.amazon.smithy.model.knowledge.HttpBindingIndex.of(context.model());
        for (var binding : new TreeMap<>(index.getResponseBindings(shape)).values()) {
          if (binding.getLocation()
              != software.amazon.smithy.model.knowledge.HttpBinding.Location.HEADER) {
            continue;
          }
          Shape target = context.model().expectShape(binding.getMember().getTarget());
          if (target.hasTrait(software.amazon.smithy.model.traits.MediaTypeTrait.class)) {
            // Media-type strings and non-scalar error headers are not
            // serialized yet (exclusions document the affected cases).
            continue;
          }
          // The lifted value comes from the serialized body document, so the
          // conversion branches on the document node the serde produced.
          String lift =
              switch (target.getType()) {
                case STRING, ENUM ->
                    "if (it->second.is_string()) header_values.emplace_back($S, "
                        + "it->second.as_string());";
                case BYTE, SHORT, INTEGER, LONG, INT_ENUM ->
                    "if (it->second.is_int()) header_values.emplace_back($S, "
                        + "std::to_string(it->second.as_int()));";
                case FLOAT, DOUBLE ->
                    "if (it->second.is_double()) header_values.emplace_back($S, "
                        + "opal::FormatDouble(it->second.as_double()));";
                case BOOLEAN ->
                    "if (it->second.is_bool()) header_values.emplace_back($S, "
                        + "it->second.as_bool() ? \"true\" : \"false\");";
                default -> null;
              };
          if (lift == null) {
            continue;
          }
          w.openBlock(
              "if (auto it = body.find($S); it != body.end()) {",
              binding.getMember().getMemberName());
          w.write(lift, binding.getLocationName());
          w.write("body.erase(it);");
          w.closeBlock("}");
        }
        w.write(
            "auto response = $L($L, \"\", \"\", std::move(body)$L);",
            errorCall,
            errorStatus(shape),
            extraArgs);
        w.write("response.headers.Set($S, error.code());", errortypeHeader);
        w.write(
            "for (const auto& [name, value] : header_values) response.headers.Set(name, value);");
        w.write("return response;");
      } else {
        // rpcv2Cbor/jsonRpc2: __type carries the fully qualified shape id.
        w.write(
            "return $L($L, $S, \"\", std::move(body)$L);",
            errorCall,
            errorStatus(shape),
            shape.getId().toString(),
            extraArgs);
      }
      w.closeBlock("}");
    }
    w.write("return $L(400, error.code(), error.message(), {}$L);", errorCall, extraArgs);
    w.closeBlock("}");
    if (!errortypeHeader.isEmpty()) {
      // restJson1: parse failures answer 400 with the SerializationException
      // error identity in the header (the malformed-request suite pins this).
      w.openBlock(
          "if (error.kind() == opal::ErrorKind::kValidation || error.kind() == "
              + "opal::ErrorKind::kSerialization) {");
      w.write("auto response = $L(400, \"\", error.message(), {}$L);", errorCall, extraArgs);
      w.write("response.headers.Set($S, \"SerializationException\");", errortypeHeader);
      w.write("return response;");
      w.closeBlock("}");
    } else {
      w.write(
          "if (error.kind() == opal::ErrorKind::kValidation || error.kind() == "
              + "opal::ErrorKind::kSerialization) return $L(400, \"SerializationException\", "
              + "error.message(), {}$L);",
          errorCall,
          extraArgs);
    }
    w.write("// Never leak internal detail on unexpected failures.");
    w.write(
        "return $L(500, \"InternalFailure\", \"internal failure\", {}$L);", errorCall, extraArgs);
    w.closeBlock("}");
    w.write("");
  }

  static List<String> sharedServerIncludes(CppContext context) {
    return List.of(
        "\"" + context.settings().includePrefix() + "/server.h\"",
        "\"" + context.settings().includePrefix() + "/serde.h\"",
        "\"smithy/server/router.h\"",
        "<memory>",
        "<utility>",
        "<vector>",
        "<string>",
        "<string_view>",
        "<utility>");
  }

  /**
   * Emits Make&lt;Error&gt;Error for every error shape any operation declares (typed detail via the
   * shape's serde, @retryable honored) plus a Parse&lt;Op&gt;Error dispatcher per operation that
   * has errors. Must run inside the client source's helpers namespace (nested in the anonymous
   * one), after {@link #writeErrorSupport}.
   */
  static void writeOperationErrorParsers(
      CppWriter w,
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    Map<String, StructureShape> errorShapes = new TreeMap<>();
    for (OperationShape operation : operations) {
      for (ShapeId errorId : operation.getErrors(service)) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        errorShapes.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
    }
    for (StructureShape shape : errorShapes.values()) {
      boolean retryable = shape.hasTrait(RetryableTrait.class);
      w.openBlock(
          "opal::Error $L(const opal::http::HttpResponse& response, ParsedError parsed) {",
          makeErrorFunction(context, shape));
      w.write("(void)response;");
      if (retryable) {
        w.write("const bool retryable = true;  // @retryable");
      } else {
        w.write("const bool retryable = parsed.status >= 500;");
      }
      // code() carries the wire-level shape name, which can differ from the
      // C++ type name when a foreign-namespace shape was disambiguated.
      w.write(
          "opal::Error error = opal::Error::Modeled($S, std::move(parsed.message), "
              + "retryable);",
          shape.getId().getName());
      // Errors with header-only payloads (or none at all) may have no body:
      // deserialize from an empty map so the typed detail still attaches.
      w.write("if (!parsed.doc.is_map()) parsed.doc = opal::Document(opal::DocumentMap{});");
      // Header-bound members are patched into the document before it
      // deserializes, so @required header members are satisfied.
      protocol.writeErrorDocPatches(w, context, shape);
      w.write(
          "auto detail = Deserialize$L(parsed.doc);",
          SerdeCodeGen.serdeFunctionSuffix(context, shape));
      w.openBlock("if (detail.ok()) {");
      w.write("error.set_detail(*std::move(detail));");
      w.closeBlock("}");
      w.write("return error;");
      w.closeBlock("}");
      w.write("");
    }
    for (OperationShape operation : operations) {
      List<ShapeId> errors = operation.getErrors(service);
      if (errors.isEmpty()) {
        continue;
      }
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // Streaming operations never see an HTTP error response — exceptions
        // arrive as event-stream messages, decoded straight through the
        // Make<Error>Error functions above — so Parse<Op>Error would be dead.
        continue;
      }
      Map<String, StructureShape> sorted = new TreeMap<>();
      for (ShapeId errorId : errors) {
        StructureShape shape =
            context.model().expectShape(errorId).asStructureShape().orElseThrow();
        sorted.put(context.cppSymbols().toSymbol(shape).getName(), shape);
      }
      // Parse<Op>Error, not Deserialize<Op>Error: the serde functions own the
      // Serialize/Deserialize<Shape> namespace, and a same-named file-local
      // helper would hide them (C++ name hiding) for shapes named <Op>Error.
      w.openBlock(
          "opal::Error $L(const opal::http::HttpResponse& response) {",
          parseErrorFunction(operation));
      w.write("ParsedError parsed = helpers::ParseError(response);");
      for (Map.Entry<String, StructureShape> entry : sorted.entrySet()) {
        w.write(
            "if (parsed.code == $S) return helpers::$L(response, std::move(parsed));",
            entry.getValue().getId().getName(),
            makeErrorFunction(context, entry.getValue()));
      }
      if (protocol.errorStatusFallback()) {
        // Statuses claimed by exactly one declared error fall back by status
        // when the response carried no error identity.
        Map<Integer, List<Map.Entry<String, StructureShape>>> byStatus = new TreeMap<>();
        for (Map.Entry<String, StructureShape> entry : sorted.entrySet()) {
          byStatus
              .computeIfAbsent(errorStatus(entry.getValue()), k -> new java.util.ArrayList<>())
              .add(entry);
        }
        for (Map.Entry<Integer, List<Map.Entry<String, StructureShape>>> entry :
            byStatus.entrySet()) {
          if (entry.getValue().size() != 1) {
            continue;
          }
          w.write(
              "if (parsed.code == \"UnknownError\" && parsed.status == $L) "
                  + "return helpers::$L(response, std::move(parsed));",
              entry.getKey(),
              makeErrorFunction(context, entry.getValue().get(0).getValue()));
        }
      }
      w.write("return helpers::GenericError(std::move(parsed));");
      w.closeBlock("}");
      w.write("");
    }
  }

  /** The one derivation of the Parse<Op>Error helper name (definition and call sites). */
  private static String parseErrorFunction(OperationShape operation) {
    return "Parse" + CppReservedWords.escape(operation.getId().getName()) + "Error";
  }

  /** The one derivation of the Make&lt;Error&gt;Error helper name, same contract. */
  static String makeErrorFunction(CppContext context, StructureShape errorShape) {
    return "Make" + context.cppSymbols().toSymbol(errorShape).getName() + "Error";
  }

  /** The expression a protocol's operation body returns for a non-success response. */
  static String errorExpression(CppContext context, ServiceShape service, OperationShape op) {
    if (op.getErrors(service).isEmpty()) {
      return "helpers::GenericError(helpers::ParseError(*response))";
    }
    return "helpers::" + parseErrorFunction(op) + "(*response)";
  }

  /**
   * If the input has @idempotencyToken members, emits a prepared copy with unset tokens filled and
   * returns the expression to use for the input from then on.
   */
  static String prepareIdempotencyTokens(
      CppWriter w, CppContext context, StructureShape input, String inputType) {
    boolean any = false;
    for (MemberShape member : input.members()) {
      if (!member.hasTrait(IdempotencyTokenTrait.class)) {
        continue;
      }
      if (!any) {
        w.write("$L prepared = input;", inputType);
        any = true;
      }
      String field = "prepared." + context.cppSymbols().toMemberName(member);
      if (member.isRequired()) {
        w.write("if ($L.empty()) $L = opal::GenerateUuidV4();", field, field);
      } else {
        w.write("if (!$L.has_value()) $L = opal::GenerateUuidV4();", field, field);
      }
    }
    return any ? "prepared" : "input";
  }

  /** String conversion for label/query/header values of simple types. */
  static String toStringExpression(
      CppContext context, MemberShape member, String valueExpr, String timestampFormat) {
    Shape target = context.model().expectShape(member.getTarget());
    return switch (target.getType()) {
      case STRING -> valueExpr;
      case ENUM -> "std::string(" + valueExpr + ".ToString())";
      case BYTE, SHORT, INTEGER, LONG, INT_ENUM ->
          "std::to_string(static_cast<std::int64_t>(" + valueExpr + "))";
      case FLOAT -> "opal::FormatFloat(" + valueExpr + ")";
      case DOUBLE -> "opal::FormatDouble(" + valueExpr + ")";
      case BOOLEAN -> "(" + valueExpr + " ? \"true\" : \"false\")";
      case TIMESTAMP -> valueExpr + ".Format(" + timestampFormat + ")";
      default ->
          throw new CodegenException("cpp-codegen: unsupported binding target " + target.getId());
    };
  }

  static StructureShape inputShape(CppContext context, OperationShape operation) {
    return context.model().expectShape(operation.getInputShape()).asStructureShape().orElseThrow();
  }

  static StructureShape outputShape(CppContext context, OperationShape operation) {
    return context.model().expectShape(operation.getOutputShape()).asStructureShape().orElseThrow();
  }

  static List<String> sharedClientIncludes(CppContext context) {
    return List.of(
        "\"" + context.settings().includePrefix() + "/client.h\"",
        "\"" + context.settings().includePrefix() + "/serde.h\"",
        "\"smithy/core/blob.h\"",
        "\"smithy/core/document_serde.h\"",
        "\"smithy/core/uuid.h\"",
        "\"smithy/http/socket_transport.h\"",
        "\"smithy/http/uri.h\"",
        "<string>",
        "<string_view>",
        "<utility>");
  }
}
