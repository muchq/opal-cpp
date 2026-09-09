// Fuzz target: CBOR bytes -> Document -> CBOR bytes. Decode must never crash;
// anything it accepts must re-encode without throwing.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "opal/cbor/cbor.h"
#include "opal/core/blob.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto blob = opal::Blob::FromString(std::string(reinterpret_cast<const char*>(data), size));
  auto doc = opal::cbor::Decode(blob);
  if (doc.ok()) {
    (void)opal::cbor::Encode(*doc);
  }
  return 0;
}
