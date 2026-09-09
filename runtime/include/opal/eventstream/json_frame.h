#ifndef OPAL_EVENTSTREAM_JSON_FRAME_H_
#define OPAL_EVENTSTREAM_JSON_FRAME_H_

#include <string>
#include <string_view>

#include "opal/core/outcome.h"
#include "opal/eventstream/frame.h"

namespace opal::eventstream {

// The negotiated JSON-text wire encoding (ADR-0018): the frame a browser
// can speak with JSON.parse alone. A client offering this subprotocol on
// the WebSocket upgrade — and only such a client — gets sessions whose
// messages travel as text frames carrying a JSON envelope instead of
// binary event-stream frames:
//
//   {"event": "<member>", "payload": {...}}        an event
//   {"exception": "<shape>", "payload": {...}}     the terminal error arm
//
// One event per text message, closed-union semantics unchanged: the
// envelope is the ONE place these member names live, this codec's twin of
// envelope.h's rule for the header names. The translation is a transport
// detail — both directions produce/consume the same eventstream::Message
// the binary wire carries, so every layer above the socket (EventStream,
// routers, generated code) never learns the wire changed.
inline constexpr std::string_view kJsonFramesSubprotocol = "smithy.eventstream.v1+json";

// Renders one envelope-bearing message as the JSON text frame. Refuses
// (Error::Validation, session untouched) what this wire cannot represent:
// a message without the ADR-0016 envelope headers, headers beyond the
// envelope's own (the JSON envelope has no header channel), a
// :content-type other than application/json (rpcv2Cbor events cannot ride
// a text frame — the mode is scoped to JSON-payload services), a payload
// that is not a JSON object, or a rendered frame over kMaxMessageBytes.
// The payload is parsed and re-encoded through the runtime's JSON codec,
// so the text frame always carries exactly one JSON dialect.
Outcome<std::string> EncodeJsonFrame(const Message& message);

// Parses one received text frame back into the Message the binary wire
// would have carried, :content-type stamped application/json. Fail-closed
// (Error::Serialization — the session is dead, mirroring a malformed
// binary frame): not a JSON object, an unknown envelope member, both or
// neither of "event"/"exception", a non-string or empty type, a missing
// or non-object "payload", or text over kMaxMessageBytes. Unknown *event
// types* are not this layer's call: they decode into a Message whose
// :event-type the generated decoder rejects terminally, exactly as in
// binary mode.
Outcome<Message> DecodeJsonFrame(std::string_view text);

}  // namespace opal::eventstream

#endif  // OPAL_EVENTSTREAM_JSON_FRAME_H_
