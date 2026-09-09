// Fuzz target: URI machinery. PercentDecode must never crash, and anything it
// accepts must survive an encode -> decode round trip; the encoders must
// accept arbitrary bytes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "smithy/http/uri.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  auto decoded = opal::http::PercentDecode(text);
  if (decoded.ok()) {
    auto reencoded = opal::http::EncodeQueryComponent(*decoded);
    auto redecoded = opal::http::PercentDecode(reencoded);
    if (!redecoded.ok() || *redecoded != *decoded) std::abort();
  }
  (void)opal::http::EncodePathSegment(text);
  (void)opal::http::EncodeGreedyPathSegment(text);
  return 0;
}
