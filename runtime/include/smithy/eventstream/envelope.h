#ifndef SMITHY_EVENTSTREAM_ENVELOPE_H_
#define SMITHY_EVENTSTREAM_ENVELOPE_H_

#include <string>
#include <string_view>

#include "smithy/core/blob.h"
#include "smithy/core/outcome.h"
#include "smithy/core/print.h"
#include "smithy/eventstream/frame.h"

namespace opal::eventstream {

// The event envelope (ADR-0016): the ONE place the header convention that
// puts typed events and exceptions onto event-stream messages lives.
// Generated streaming code, future @eventHeader support, and any future
// HTTP-chunked event-stream wire all mint and parse through these helpers —
// nothing else spells the header names.
//
// The convention, per the format's own headers:
//   :message-type    "event" or "exception"
//   :event-type      the event union member name (events only)
//   :exception-type  the error shape name (exceptions only)
//   :content-type    the payload's content type; may be absent
// The payload is the protocol's serialized bytes; this layer does not
// interpret them.

// The reserved :event-type carrying output-structure members outside the
// stream, sent before any event (ADR-0016). The leading ':' keeps it out of
// the Smithy member-name space, so it can never collide with a modeled
// event.
inline constexpr std::string_view kInitialResponseEventType = ":initial-response";

// One parsed envelope: which kind of message arrived, its type name
// (:event-type or :exception-type by kind), the payload's content type
// ("" when the message carried none), and the payload bytes.
struct EventEnvelope {
  enum class Kind { kEvent, kException };
  Kind kind = Kind::kEvent;
  std::string type;
  std::string content_type;
  Blob payload;
  friend bool operator==(const EventEnvelope&, const EventEnvelope&) = default;

  // Debug rendering (smithy/core/print.h).
  void AppendDebugTo(std::string& out) const {
    out += kind == Kind::kEvent ? "EventEnvelope(event, " : "EventEnvelope(exception, ";
    DebugAppend(out, type);
    out += ", content_type=";
    DebugAppend(out, content_type);
    out += ", payload=";
    payload.AppendDebugTo(out);
    out += ')';
  }
};

// Mints an event message: :message-type "event", :event-type, and
// :content-type — omitted when `content_type` is empty, the encode-side
// mirror of ParseEnvelope tolerating its absence.
Message MakeEventMessage(std::string_view event_type, std::string_view content_type, Blob payload);

// Mints an exception message: :message-type "exception", :exception-type,
// and :content-type (omitted when empty), for the terminal error message a
// handler's failure sends before the close (ADR-0016).
Message MakeExceptionMessage(std::string_view exception_type, std::string_view content_type,
                             Blob payload);

// Parses a received message's envelope. Error::Serialization when
// :message-type is missing, not a string, or neither "event" nor
// "exception" (unknown kinds are hard errors: silently skipping a message
// would desynchronize a typed stream), and when the kind's type header is
// missing or not a string. An absent :content-type parses as "" — the
// protocol layer already knows its content type; a present one of the
// wrong wire type is an error.
Outcome<EventEnvelope> ParseEnvelope(const Message& message);

}  // namespace opal::eventstream

#endif  // SMITHY_EVENTSTREAM_ENVELOPE_H_
