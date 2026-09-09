// Fuzz target: gzip, both directions, under fuzzed input slicing (the
// issue #109 seam). Decompress must never crash on arbitrary bytes and
// must agree with its slice-fed twin — same verdict, same bytes — for any
// slice bound; whatever compress emits under any slicing must decompress
// back to the exact input. The output cap keeps decompression bombs
// bounded, so the harness also exercises the mid-stream limit.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "opal/compression/gzip.h"
#include "opal/compression/gzip_test_peer.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  // Byte one steers the slice bound (1..256) so boundaries land everywhere;
  // the rest is the payload.
  const std::size_t max_feed = std::size_t{data[0]} + 1;
  const std::string_view payload(reinterpret_cast<const char*>(data + 1), size - 1);
  constexpr std::size_t kOutputCap = std::size_t{1} << 20;

  // Differential decompress: slicing must not change the verdict or bytes.
  const auto whole = opal::GzipDecompress(payload, kOutputCap);
  const auto sliced = opal::internal::GzipDecompressChunked(payload, kOutputCap, max_feed);
  if (whole.ok() != sliced.ok()) std::abort();        // slicing changed the verdict
  if (whole.ok() && *whole != *sliced) std::abort();  // slicing changed the bytes

  // Round trip: compress under the fuzzed slicing, read back both ways.
  const auto packed = opal::internal::GzipCompressChunked(payload, max_feed);
  if (!packed.ok()) std::abort();  // compress accepts any bytes
  const auto restored = opal::GzipDecompress(*packed, kOutputCap);
  if (!restored.ok()) std::abort();        // own output must decompress
  if (*restored != payload) std::abort();  // and round-trip exactly
  const auto re_sliced = opal::internal::GzipDecompressChunked(*packed, kOutputCap, max_feed);
  if (!re_sliced.ok() || *re_sliced != payload) std::abort();
  return 0;
}
