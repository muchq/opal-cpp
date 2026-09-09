// Fuzz target: the JSON-text frame codec (ADR-0018). Decode is
// attacker-reachable text — any browser (or anything claiming to be one)
// types it straight at a negotiated session — so it must never crash, and
// an accepted envelope must re-encode: whatever Decode admits, Encode can
// represent (the fail-closed rules are symmetric or they are wrong). The
// round trip is canonicalizing, not byte-identical — decode re-encodes the
// payload through the runtime's JSON codec — so the pinned property is
// Message-level: re-encoding the decoded Message and decoding again must
// yield the same Message (the second trip is a fixed point).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "opal/eventstream/json_frame.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto message = opal::eventstream::DecodeJsonFrame(text);
  if (!message.ok()) {
    return 0;
  }
  const auto reencoded = opal::eventstream::EncodeJsonFrame(*message);
  if (!reencoded.ok()) std::abort();  // decoded but not re-encodable: bounds asymmetry
  const auto again = opal::eventstream::DecodeJsonFrame(*reencoded);
  if (!again.ok()) std::abort();         // the canonical form must decode
  if (*again != *message) std::abort();  // and be a fixed point
  return 0;
}
