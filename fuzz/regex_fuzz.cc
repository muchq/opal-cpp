// Fuzz target: the linear-time @pattern engine. Compile must never crash on
// arbitrary pattern bytes, and anything it accepts must Search arbitrary
// input in bounded time (the harness splits the record into pattern and
// text at the first NUL). ReDoS-resistance is the engine's contract, so a
// hang here is a finding, not flakiness.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "opal/core/regex.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view record(reinterpret_cast<const char*>(data), size);
  const std::size_t split = record.find('\0');
  const std::string_view pattern =
      split == std::string_view::npos ? record : record.substr(0, split);
  const std::string_view text = split == std::string_view::npos ? "" : record.substr(split + 1);
  auto re = opal::Regex::Compile(pattern);
  if (re.ok()) {
    (void)re->Search(text);
    (void)re->Search(pattern);
  }
  return 0;
}
