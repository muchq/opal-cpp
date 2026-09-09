// Fuzz target: the jsonRpc2 stream codec (ADR-0023). Classification is
// attacker-reachable text — any browser types envelopes straight at the
// shared endpoint — and never fails: whatever the grammar refuses must
// come back as kViolation carrying a reserved code and a fixed reason
// (which must render as a well-formed terminal via
// EncodeJsonRpcViolationResponse, itself decodable). An admitted event
// must re-encode: EncodeJsonRpcNotification represents whatever Decode
// classifies kEvent, and the round trip is a Message-level fixed point
// (decode re-encodes the payload through the runtime's JSON codec, so
// bytes may canonicalize once but Messages may not drift).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "opal/core/document.h"
#include "opal/eventstream/jsonrpc_frame.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const opal::Document id(1);
  const auto frame = opal::eventstream::DecodeJsonRpcStreamFrame(text, id);
  using Kind = opal::eventstream::JsonRpcStreamFrame::Kind;
  if (frame.kind == Kind::kViolation) {
    if (frame.code != -32700 && frame.code != -32600) std::abort();  // reserved codes only
    if (frame.reason.empty()) std::abort();  // the terminal's message member
    const auto terminal =
        opal::eventstream::EncodeJsonRpcViolationResponse(frame.code, frame.reason, id);
    // The server answers this text on the wire; the codec itself must
    // classify it (as a response envelope, never a violation of its own).
    const auto answered = opal::eventstream::DecodeJsonRpcStreamFrame(terminal, id);
    if (answered.kind != Kind::kException) std::abort();
    return 0;
  }
  if (frame.kind != Kind::kEvent) {
    return 0;  // kResult/kException carry no notification to re-encode
  }
  const auto reencoded = opal::eventstream::EncodeJsonRpcNotification(frame.message, id);
  if (!reencoded.ok()) std::abort();  // classified kEvent but not representable: asymmetry
  const auto again = opal::eventstream::DecodeJsonRpcStreamFrame(*reencoded, id);
  if (again.kind != Kind::kEvent) std::abort();      // the canonical form must classify back
  if (again.message != frame.message) std::abort();  // and be a fixed point
  return 0;
}
