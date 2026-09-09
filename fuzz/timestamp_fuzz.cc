// Fuzz target: the three wire-facing timestamp parsers (epoch-seconds,
// RFC 3339 date-time, IMF-fixdate). Parsing arbitrary bytes must never
// crash, and anything accepted must reformat and reparse to the same
// instant — a fixed point, pinning the Format/Parse pair the issue #109
// A-2 corruption lived in. The input's first eight bytes also derive a raw
// int64 instant, so Format(kEpochSeconds) is exercised over the whole
// unchecked domain (it must be total) with exact round-trips asserted
// inside the representable window.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "opal/core/timestamp.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  for (const auto format : {opal::TimestampFormat::kEpochSeconds, opal::TimestampFormat::kDateTime,
                            opal::TimestampFormat::kHttpDate}) {
    const auto parsed = opal::Timestamp::Parse(text, format);
    if (!parsed.ok()) continue;
    const std::string canonical = parsed->Format(format);
    const auto again = opal::Timestamp::Parse(canonical, format);
    if (!again.ok()) std::abort();        // own output must parse
    if (*again != *parsed) std::abort();  // and be a fixed point
  }

  if (size >= 8) {
    std::int64_t ms = 0;
    std::memcpy(&ms, data, sizeof ms);
    // Total over the unchecked domain: must not crash or trip UBSan even
    // at the int64 extremes (the civil decomposition is not involved).
    const std::string rendered =
        opal::Timestamp::FromEpochMilliseconds(ms).Format(opal::TimestampFormat::kEpochSeconds);
    // Inside the checked window the text must read back exactly; outside
    // it the checked parser refuses by design.
    constexpr std::int64_t kMinRepresentableMs = -62167219200000;
    constexpr std::int64_t kMaxRepresentableMs = 253402300799999;
    const auto read = opal::Timestamp::Parse(rendered, opal::TimestampFormat::kEpochSeconds);
    if (ms >= kMinRepresentableMs && ms <= kMaxRepresentableMs) {
      if (!read.ok()) std::abort();                        // in-window text must parse
      if (read->epoch_milliseconds() != ms) std::abort();  // exactly
    }
  }
  return 0;
}
