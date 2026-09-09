#include "opal/eventstream/json_frame.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

#include "opal/core/document.h"
#include "opal/core/error.h"
#include "opal/eventstream/envelope.h"
#include "opal/json/json.h"

namespace opal::eventstream {
namespace {

constexpr std::string_view kEventMember = "event";
constexpr std::string_view kExceptionMember = "exception";
constexpr std::string_view kPayloadMember = "payload";
constexpr std::string_view kJsonContentType = "application/json";

// Encode-side refusals are Validation (the session is untouched and stays
// usable — the WebSocket Send contract); decode-side are Serialization
// (the wire produced something malformed: the session is dead).
Error CannotEncode(std::string what) {
  return Error::Validation("eventstream json frame: " + std::move(what));
}

Error Malformed(std::string what) {
  return Error::Serialization("eventstream json frame: " + std::move(what));
}

// The media type of a :content-type value: parameters stripped, trimmed,
// lowercased. Local on purpose — http's MediaTypeOf lives above this
// library in the dependency order.
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

}  // namespace

Outcome<std::string> EncodeJsonFrame(const Message& message) {
  auto envelope = ParseEnvelope(message);
  if (!envelope.ok()) {
    // Only envelope-bearing messages can ride this wire; surface the parse
    // failure at the encode kind the Send contract promises.
    return CannotEncode(envelope.error().message());
  }
  // The JSON envelope has no header channel: the only headers this wire can
  // carry are the envelope's own, and ParseEnvelope just proved those are
  // present — so a count beyond them means an extra (or duplicated) header
  // that must be refused rather than dropped: encode refuses what the wire
  // cannot represent (ADR-0014's rule). Counting also keeps the header
  // names spelled in envelope.cc alone.
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
  DocumentMap fields;
  fields.emplace(envelope->kind == EventEnvelope::Kind::kEvent ? kEventMember : kExceptionMember,
                 Document(std::move(envelope->type)));
  fields.emplace(kPayloadMember, std::move(*payload));
  std::string text = json::Encode(Document(std::move(fields)));
  if (text.size() > kMaxMessageBytes) {
    return CannotEncode("message over the 16 MiB limit");
  }
  return text;
}

Outcome<Message> DecodeJsonFrame(std::string_view text) {
  if (text.size() > kMaxMessageBytes) {
    return Malformed("message over the 16 MiB limit");
  }
  const auto envelope = json::Decode(text);
  if (!envelope.ok()) {
    return Malformed("text frame is not JSON: " + envelope.error().message());
  }
  if (!envelope->is_map()) {
    return Malformed("envelope is not a JSON object");
  }
  // Exactly the envelope's members, fail-closed: an unknown member fails
  // the session (the transpose of binary mode's posture on malformed
  // frames), so a peer speaking some richer dialect is refused instead of
  // half-understood.
  for (const auto& [name, value] : envelope->as_map()) {
    if (name != kEventMember && name != kExceptionMember && name != kPayloadMember) {
      return Malformed("unknown envelope member: " + name);
    }
  }
  const Document* event = envelope->Find(kEventMember);
  const Document* exception = envelope->Find(kExceptionMember);
  if ((event != nullptr) == (exception != nullptr)) {
    return Malformed(event != nullptr ? "envelope carries both \"event\" and \"exception\""
                                      : "envelope carries neither \"event\" nor \"exception\"");
  }
  const Document& type = event != nullptr ? *event : *exception;
  if (!type.is_string() || type.as_string().empty()) {
    return Malformed(std::string(event != nullptr ? kEventMember : kExceptionMember) +
                     " is not a non-empty string");
  }
  const Document* payload = envelope->Find(kPayloadMember);
  if (payload == nullptr) {
    return Malformed("envelope is missing \"payload\"");
  }
  if (!payload->is_map()) {
    return Malformed("\"payload\" is not a JSON object");
  }
  // Re-encoded through the runtime's JSON codec: generated serde sees one
  // dialect regardless of what the peer typed.
  Blob payload_bytes = Blob::FromString(json::Encode(*payload));
  return event != nullptr
             ? MakeEventMessage(type.as_string(), kJsonContentType, std::move(payload_bytes))
             : MakeExceptionMessage(type.as_string(), kJsonContentType, std::move(payload_bytes));
}

}  // namespace opal::eventstream
