#include "opal/eventstream/jsonrpc_frame.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

#include "opal/core/error.h"
#include "opal/eventstream/envelope.h"
#include "opal/json/json.h"

namespace opal::eventstream {
namespace {

constexpr std::string_view kVersionMember = "jsonrpc";
constexpr std::string_view kVersionValue = "2.0";
constexpr std::string_view kMethodMember = "method";
constexpr std::string_view kParamsMember = "params";
constexpr std::string_view kIdMember = "id";
constexpr std::string_view kPayloadMember = "payload";
constexpr std::string_view kResultMember = "result";
constexpr std::string_view kErrorMember = "error";
constexpr std::string_view kCodeMember = "code";
constexpr std::string_view kMessageMember = "message";
constexpr std::string_view kDataMember = "data";
constexpr std::string_view kTypeMember = "__type";
constexpr std::string_view kFallbackExceptionType = "JsonRpcError";
constexpr std::string_view kViolationExceptionType = "SerializationException";
constexpr std::string_view kJsonContentType = "application/json";
constexpr int kParseErrorCode = -32700;
constexpr int kInvalidRequestCode = -32600;

// Encode-side refusals stay Validation (the session is untouched and
// usable — the WebSocket Send contract). Decode-side failures are not
// errors at all: they classify as kViolation frames, whose FIXED reason
// strings are wire text (the server answers them as the terminal error),
// pinned by the conformance suite.
Error CannotEncode(std::string what) {
  return Error::Validation("jsonrpc stream: " + std::move(what));
}

JsonRpcStreamFrame Violation(int code, std::string_view reason) {
  JsonRpcStreamFrame frame;
  frame.kind = JsonRpcStreamFrame::Kind::kViolation;
  frame.code = code;
  frame.reason = std::string(reason);
  return frame;
}

// The media type of a :content-type value: parameters stripped, trimmed,
// lowercased. Local on purpose — http's MediaTypeOf lives above this
// library in the dependency order (json_frame.cc's twin).
std::string MediaType(std::string_view content_type) {
  const std::size_t semicolon = content_type.find(';');
  if (semicolon != std::string_view::npos) {
    content_type = content_type.substr(0, semicolon);
  }
  while (!content_type.empty() && (content_type.front() == ' ' || content_type.front() == '\t')) {
    content_type.remove_prefix(1);
  }
  while (!content_type.empty() && (content_type.back() == ' ' || content_type.back() == '\t')) {
    content_type.remove_suffix(1);
  }
  std::string lowered(content_type);
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered;
}

JsonRpcStreamFrame DecodeNotification(const Document& envelope, const Document& id) {
  const Document& method = *envelope.Find(kMethodMember);
  if (!method.is_string() || method.as_string().empty()) {
    return Violation(kInvalidRequestCode, "\"method\" is not a non-empty string");
  }
  const Document* params = envelope.Find(kParamsMember);
  if (params == nullptr || !params->is_map()) {
    return Violation(kInvalidRequestCode,
                     "notification \"params\" is missing or not a JSON object");
  }
  // Exactly {"id", "payload"}, fail-closed: an extra member means a dialect
  // this stream does not speak, refused instead of half-understood.
  for (const auto& [name, value] : params->as_map()) {
    if (name != kIdMember && name != kPayloadMember) {
      return Violation(kInvalidRequestCode, "unknown notification params member: " + name);
    }
  }
  const Document* echoed = params->Find(kIdMember);
  if (echoed == nullptr) {
    return Violation(kInvalidRequestCode, "notification params carry no \"id\" echo");
  }
  if (*echoed != id) {
    return Violation(kInvalidRequestCode,
                     "notification for a different call's id (one stream per socket)");
  }
  const Document* payload = params->Find(kPayloadMember);
  if (payload == nullptr || !payload->is_map()) {
    return Violation(kInvalidRequestCode,
                     "notification \"payload\" is missing or not a JSON object");
  }
  JsonRpcStreamFrame frame;
  frame.kind = JsonRpcStreamFrame::Kind::kEvent;
  // Re-encoded through the runtime's JSON codec: generated serde sees one
  // dialect regardless of what the peer typed.
  frame.message = MakeEventMessage(method.as_string(), kJsonContentType,
                                   Blob::FromString(json::Encode(*payload)));
  return frame;
}

JsonRpcStreamFrame DecodeResponse(const Document& envelope, const Document& id) {
  const Document* echoed = envelope.Find(kIdMember);
  if (echoed == nullptr) {
    return Violation(kInvalidRequestCode, "response envelope carries no \"id\"");
  }
  if (*echoed != id) {
    return Violation(kInvalidRequestCode,
                     "response for a different call's id (one stream per socket)");
  }
  const Document* result = envelope.Find(kResultMember);
  const Document* error = envelope.Find(kErrorMember);
  if ((result != nullptr) == (error != nullptr)) {
    return Violation(kInvalidRequestCode,
                     result != nullptr ? "response carries both \"result\" and \"error\""
                                       : "envelope is neither a notification nor a response");
  }
  JsonRpcStreamFrame frame;
  if (result != nullptr) {
    // The terminal result — the stream's clean end. Its value is preserved
    // but not policed: today's servers answer {}, and whatever rides here
    // when initial-response support lands is terminal either way.
    frame.kind = JsonRpcStreamFrame::Kind::kResult;
    frame.result = *result;
    return frame;
  }
  if (!error->is_map()) {
    return Violation(kInvalidRequestCode, "\"error\" is not a JSON object");
  }
  for (const auto& [name, value] : error->as_map()) {
    if (name != kCodeMember && name != kMessageMember && name != kDataMember) {
      return Violation(kInvalidRequestCode, "unknown error member: " + name);
    }
  }
  const Document* code = error->Find(kCodeMember);
  if (code == nullptr || !code->is_int()) {
    return Violation(kInvalidRequestCode, "error \"code\" is missing or not an integer");
  }
  const Document* message = error->Find(kMessageMember);
  if (message != nullptr && !message->is_string()) {
    return Violation(kInvalidRequestCode, "error \"message\" is not a string");
  }
  const Document* data = error->Find(kDataMember);
  if (data != nullptr && !data->is_map()) {
    return Violation(kInvalidRequestCode, "error \"data\" is not a JSON object");
  }
  DocumentMap payload = data != nullptr ? data->as_map() : DocumentMap();
  // The unary client's fallback, mirrored: an error message member fills a
  // data object that carries none.
  if (message != nullptr && !message->as_string().empty() &&
      payload.find(kMessageMember) == payload.end()) {
    payload.emplace(kMessageMember, *message);
  }
  const Document* type = data != nullptr ? data->Find(kTypeMember) : nullptr;
  const bool typed = type != nullptr && type->is_string() && !type->as_string().empty();
  frame.kind = JsonRpcStreamFrame::Kind::kException;
  frame.message =
      MakeExceptionMessage(typed ? type->as_string() : kFallbackExceptionType, kJsonContentType,
                           Blob::FromString(json::Encode(Document(std::move(payload)))));
  return frame;
}

}  // namespace

Outcome<std::string> EncodeJsonRpcNotification(const Message& message, const Document& id) {
  auto envelope = ParseEnvelope(message);
  if (!envelope.ok()) {
    // Only envelope-bearing messages can ride this wire; surface the parse
    // failure at the encode kind the Send contract promises.
    return CannotEncode(envelope.error().message());
  }
  if (envelope->kind != EventEnvelope::Kind::kEvent) {
    return CannotEncode(
        "exceptions do not ride notifications: the terminal error envelope is the generated "
        "serve path's business (ADR-0023)");
  }
  // No header channel beyond the envelope's own — json_frame.cc's counting
  // argument, verbatim: ParseEnvelope proved the envelope headers are
  // present, so any extra count is a header this wire cannot represent.
  const std::size_t envelope_headers = envelope->content_type.empty() ? 2 : 3;
  if (message.headers.size() != envelope_headers) {
    return CannotEncode("message carries headers beyond the envelope's own");
  }
  if (!envelope->content_type.empty() && MediaType(envelope->content_type) != kJsonContentType) {
    return CannotEncode("content type is not application/json: " + envelope->content_type);
  }
  auto payload = json::Decode(std::string_view(
      reinterpret_cast<const char*>(message.payload.data()), message.payload.size()));
  if (!payload.ok() || !payload->is_map()) {
    return CannotEncode("payload is not a JSON object");
  }
  DocumentMap params;
  params.emplace(kIdMember, id);
  params.emplace(kPayloadMember, std::move(*payload));
  DocumentMap fields;
  fields.emplace(kVersionMember, Document(std::string(kVersionValue)));
  fields.emplace(kMethodMember, Document(std::move(envelope->type)));
  fields.emplace(kParamsMember, Document(std::move(params)));
  std::string text = json::Encode(Document(std::move(fields)));
  if (text.size() > kMaxMessageBytes) {
    return CannotEncode("message over the 16 MiB limit");
  }
  return text;
}

JsonRpcStreamFrame DecodeJsonRpcStreamFrame(std::string_view text, const Document& id) {
  if (text.size() > kMaxMessageBytes) {
    return Violation(kInvalidRequestCode, "message over the 16 MiB limit");
  }
  const auto envelope = json::Decode(text);
  if (!envelope.ok()) {
    // Fixed reason on purpose (no decoder detail): this string is wire text.
    return Violation(kParseErrorCode, "text frame is not JSON");
  }
  if (!envelope->is_map()) {
    return Violation(kInvalidRequestCode, "envelope is not a JSON object");
  }
  // Only the members the two envelope shapes own, fail-closed — a peer
  // speaking some richer dialect is refused instead of half-understood.
  for (const auto& [name, value] : envelope->as_map()) {
    if (name != kVersionMember && name != kMethodMember && name != kParamsMember &&
        name != kIdMember && name != kResultMember && name != kErrorMember) {
      return Violation(kInvalidRequestCode, "unknown envelope member: " + name);
    }
  }
  const Document* version = envelope->Find(kVersionMember);
  if (version == nullptr || !version->is_string() || version->as_string() != kVersionValue) {
    return Violation(kInvalidRequestCode, "expected jsonrpc: \"2.0\"");
  }
  if (envelope->Find(kMethodMember) != nullptr) {
    if (envelope->Find(kIdMember) != nullptr) {
      // A request envelope: this stream's call already opened, and one
      // stream per socket means no second call can.
      return Violation(kInvalidRequestCode, "a request envelope after the opening call");
    }
    if (envelope->Find(kResultMember) != nullptr || envelope->Find(kErrorMember) != nullptr) {
      return Violation(kInvalidRequestCode, "envelope mixes a notification with a response");
    }
    return DecodeNotification(*envelope, id);
  }
  return DecodeResponse(*envelope, id);
}

std::string EncodeJsonRpcViolationResponse(int code, std::string_view reason, const Document& id) {
  DocumentMap data;
  data.emplace(kTypeMember, Document(std::string(kViolationExceptionType)));
  DocumentMap error;
  error.emplace(kCodeMember, Document(code));
  error.emplace(kDataMember, Document(std::move(data)));
  error.emplace(kMessageMember, Document(std::string(reason)));
  DocumentMap envelope;
  envelope.emplace(kVersionMember, Document(std::string(kVersionValue)));
  envelope.emplace(kErrorMember, Document(std::move(error)));
  envelope.emplace(kIdMember, id);
  return json::Encode(Document(std::move(envelope)));
}

}  // namespace opal::eventstream
