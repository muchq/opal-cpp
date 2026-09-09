// Fuzz target: the event-stream framing codec (ADR-0014). Decode must never
// crash on arbitrary bytes, an accepted frame must re-encode byte-exactly
// (the CRCs make the mapping bijective), and every strict prefix of a valid
// frame must ask for more bytes rather than error — the incremental
// contract every transport will rely on.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "opal/eventstream/frame.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto decoded = opal::eventstream::DecodeMessage(text);
  if (!decoded.ok() || !decoded->has_value()) {
    return 0;
  }
  const auto& frame = **decoded;
  const auto reencoded = opal::eventstream::EncodeMessage(frame.message);
  if (!reencoded.ok()) std::abort();  // decoded but not re-encodable: bounds asymmetry
  if (*reencoded != text.substr(0, frame.bytes_consumed)) std::abort();  // not byte-identical
  // Every strict prefix must ask for more bytes. Prefixes have only two
  // behavior classes today (< 12 bytes: no prelude; >= 12: valid prelude,
  // buffer-shorter-than-total fires before any content is touched), so past
  // 4 KiB sweep the prelude region, ~64 deterministic interior samples, and
  // the one-byte-short boundary instead of every offset — same property,
  // bounded calls per input, and no RNG so crashes reproduce from the input
  // file alone. The exhaustive sweep lives in the unit suite.
  const std::size_t n = frame.bytes_consumed;  // always >= 16
  const auto expect_more = [&](std::size_t length) {
    const auto prefix = opal::eventstream::DecodeMessage(text.substr(0, length));
    if (!prefix.ok() || prefix->has_value()) std::abort();  // prefix must ask for more
  };
  const std::size_t stride = n <= 4096 ? 1 : n / 64;
  for (std::size_t length = 0; length < n && length <= 16; ++length) expect_more(length);
  for (std::size_t length = 17; length < n; length += stride) expect_more(length);
  expect_more(n - 1);
  return 0;
}
