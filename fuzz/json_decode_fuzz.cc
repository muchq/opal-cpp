// Fuzz target: JSON text -> Document -> JSON text. Decode must never crash;
// anything it accepts must re-encode without throwing.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "opal/json/json.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  auto doc = opal::json::Decode(text);
  if (doc.ok()) {
    (void)opal::json::Encode(*doc);
  }
  return 0;
}
