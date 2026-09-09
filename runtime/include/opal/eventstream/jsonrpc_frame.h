#ifndef OPAL_EVENTSTREAM_JSONRPC_FRAME_H_
#define OPAL_EVENTSTREAM_JSONRPC_FRAME_H_

#include <string>
#include <string_view>

#include "opal/core/document.h"
#include "opal/core/outcome.h"
#include "opal/eventstream/frame.h"

namespace opal::eventstream {

// The jsonRpc2 stream wire (ADR-0023): text JSON-RPC 2.0 envelopes end to
// end, no smithy-specific framing. After the opening request envelope
// (generated code's business, like every terminal envelope), what crosses
// mid-stream is:
//
//   {"jsonrpc": "2.0", "method": "<member>",
//    "params": {"id": <opening id>, "payload": {...}}}    an event
//   {"jsonrpc": "2.0", "result": ..., "id": <opening id>} the clean end
//   {"jsonrpc": "2.0", "error": {"code": ..., "message": ...,
//    "data": {..., "__type": "<shape>"}}, "id": ...}      the terminal error
//
// This codec is the ONE place the mid-stream grammar lives — json_frame.h's
// rule, applied to the third wire. Both directions trade in the same
// eventstream::Message the binary wire carries; JsonRpcStreamSocket wears
// it over any WebSocket so every layer above stays wire-blind.

// One received stream frame, classified. kEvent and kException carry the
// envelope-bearing Message the binary wire would have carried
// (:content-type stamped application/json); kResult is the terminal result
// envelope — the stream's clean end, with the result document preserved
// for the day ADR-0016's initial-response deferral lands. kViolation is an
// envelope-level failure: the reserved code (-32700 for unparseable text,
// -32600 for everything else) and its FIXED reason string — wire text a
// server answers as the terminal error for the opening id
// (EncodeJsonRpcViolationResponse), pinned by the conformance suite, so no
// decoder detail ever leaks into it. Both ends treat a violation as
// session-fatal (JsonRpcStreamSocket closes; the server answers first).
struct JsonRpcStreamFrame {
  enum class Kind { kEvent, kException, kResult, kViolation };
  Kind kind = Kind::kEvent;
  Message message;
  Document result;
  int code = 0;
  std::string reason;
};

// Renders one event message as the notification text frame, echoing the
// opening call's id inside params (the eth_subscribe shape). Refuses
// (Error::Validation, session untouched) what this wire cannot represent:
// a message without the ADR-0016 envelope headers, an exception message
// (terminal envelopes are minted by the generated serve path, which owns
// the @httpError table — the wrapper never guesses a code), headers
// beyond the envelope's own, a :content-type other than application/json,
// a payload that is not a JSON object, or a rendered frame over
// kMaxMessageBytes.
Outcome<std::string> EncodeJsonRpcNotification(const Message& message, const Document& id);

// Parses and classifies one received text frame against the opening call's
// id. Classification never fails — a frame the grammar refuses is itself a
// classification (kViolation): not JSON (-32700); not an object, a wrong
// or missing jsonrpc version, an unknown top-level member, a request
// envelope (a method member with a top-level id — one stream per socket),
// a notification whose params is not {"id", "payload"} with the opening id
// and an object payload, a response for an id that is not the opening
// call's, both or neither of result/error, a malformed error object, or an
// oversized frame (all -32600). An error object's data.__type names the
// exception type when present; otherwise the type falls back to
// "JsonRpcError" (matching no modeled shape, so generated decoders surface
// it as the generic terminal error), and an error message member is copied
// into the payload's "message" when the data carries none — the unary
// client's fallback, mirrored.
JsonRpcStreamFrame DecodeJsonRpcStreamFrame(std::string_view text, const Document& id);

// The terminal error envelope a server answers an envelope-level violation
// with, byte-identical in shape to the generated unary refusals:
// {"error":{"code":<code>,"data":{"__type":"SerializationException"},
// "message":<reason>},"id":<id>,"jsonrpc":"2.0"} (sorted, compact).
std::string EncodeJsonRpcViolationResponse(int code, std::string_view reason, const Document& id);

}  // namespace opal::eventstream

#endif  // OPAL_EVENTSTREAM_JSONRPC_FRAME_H_
