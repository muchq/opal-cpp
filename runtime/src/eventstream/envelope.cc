#include "smithy/eventstream/envelope.h"

#include <utility>
#include <variant>

#include "smithy/core/error.h"

namespace opal::eventstream {
namespace {

constexpr std::string_view kMessageTypeHeader = ":message-type";
constexpr std::string_view kEventTypeHeader = ":event-type";
constexpr std::string_view kExceptionTypeHeader = ":exception-type";
constexpr std::string_view kContentTypeHeader = ":content-type";
constexpr std::string_view kEventMessageType = "event";
constexpr std::string_view kExceptionMessageType = "exception";

// The parse-side rejection, prefixed once (the frame codec's Malformed
// shape).
Error Malformed(std::string what) {
  return Error::Serialization("eventstream envelope: " + std::move(what));
}

Message MakeMessage(std::string_view message_type, std::string_view type_header,
                    std::string_view type, std::string_view content_type, Blob payload) {
  Message message;
  message.headers.push_back({std::string(kMessageTypeHeader), std::string(message_type)});
  message.headers.push_back({std::string(type_header), std::string(type)});
  if (!content_type.empty()) {
    message.headers.push_back({std::string(kContentTypeHeader), std::string(content_type)});
  }
  message.payload = std::move(payload);
  return message;
}

}  // namespace

Message MakeEventMessage(std::string_view event_type, std::string_view content_type, Blob payload) {
  return MakeMessage(kEventMessageType, kEventTypeHeader, event_type, content_type,
                     std::move(payload));
}

Message MakeExceptionMessage(std::string_view exception_type, std::string_view content_type,
                             Blob payload) {
  return MakeMessage(kExceptionMessageType, kExceptionTypeHeader, exception_type, content_type,
                     std::move(payload));
}

Outcome<EventEnvelope> ParseEnvelope(const Message& message) {
  const HeaderValue* message_type = message.FindHeader(kMessageTypeHeader);
  if (message_type == nullptr) {
    return Malformed("missing :message-type header");
  }
  const std::string* message_type_text = std::get_if<std::string>(message_type);
  if (message_type_text == nullptr) {
    return Malformed(":message-type header is not a string");
  }

  EventEnvelope envelope;
  std::string_view type_header;
  if (*message_type_text == kEventMessageType) {
    envelope.kind = EventEnvelope::Kind::kEvent;
    type_header = kEventTypeHeader;
  } else if (*message_type_text == kExceptionMessageType) {
    envelope.kind = EventEnvelope::Kind::kException;
    type_header = kExceptionTypeHeader;
  } else {
    return Malformed("unknown :message-type: " + *message_type_text);
  }

  const HeaderValue* type = message.FindHeader(type_header);
  if (type == nullptr) {
    return Malformed("missing " + std::string(type_header) + " header");
  }
  const std::string* type_text = std::get_if<std::string>(type);
  if (type_text == nullptr) {
    return Malformed(std::string(type_header) + " header is not a string");
  }
  envelope.type = *type_text;

  if (const HeaderValue* content_type = message.FindHeader(kContentTypeHeader);
      content_type != nullptr) {
    const std::string* content_type_text = std::get_if<std::string>(content_type);
    if (content_type_text == nullptr) {
      return Malformed(":content-type header is not a string");
    }
    envelope.content_type = *content_type_text;
  }

  envelope.payload = message.payload;
  return envelope;
}

}  // namespace opal::eventstream
