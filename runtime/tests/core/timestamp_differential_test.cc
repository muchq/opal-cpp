// Differential test: our hand-rolled timestamp formatting/parsing against the
// C library's gmtime over random epochs. Shakes out calendar math bugs that
// example-based tests miss.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <random>
#include <string>
#include <string_view>

#include "smithy/core/timestamp.h"

namespace opal {
namespace {

bool GmTime(std::int64_t epoch_seconds, std::tm* out) {
  const auto time = static_cast<std::time_t>(epoch_seconds);
  return gmtime_r(&time, out) != nullptr;
}

TEST(TimestampDifferentialTest, DateTimeMatchesGmtimeOverRandomEpochs) {
  std::mt19937_64 rng(20260706);
  // Year ~68 CE .. ~2920 CE: RFC 3339 date-time only defines four-digit years
  // (0000-9999), and this range keeps every sample inside it.
  std::uniform_int_distribution<std::int64_t> epochs(-60'000'000'000LL, 30'000'000'000LL);
  for (int i = 0; i < 2000; ++i) {
    const std::int64_t seconds = epochs(rng);
    std::tm reference{};
    if (!GmTime(seconds, &reference)) {
      continue;  // libc range limits are not what we're testing
    }
    // Sized for gcc's -Wformat-truncation worst case (full int widths), not
    // the real bounded output (the epoch range keeps years at four digits).
    char expected[80];
    std::snprintf(expected, sizeof(expected), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  reference.tm_year + 1900, reference.tm_mon + 1, reference.tm_mday,
                  reference.tm_hour, reference.tm_min, reference.tm_sec);

    const auto ts = Timestamp::FromEpochMilliseconds(seconds * 1000);
    ASSERT_EQ(ts.Format(TimestampFormat::kDateTime), expected) << "epoch " << seconds;

    // And parsing our own output must return the exact instant.
    const auto parsed = Timestamp::Parse(expected, TimestampFormat::kDateTime);
    ASSERT_TRUE(parsed.ok()) << expected;
    ASSERT_EQ(parsed->epoch_milliseconds(), seconds * 1000) << expected;
  }
}

TEST(TimestampDifferentialTest, FourDigitYearBoundariesRoundTrip) {
  // 9999-12-31T23:59:59Z, the top of the RFC 3339 four-digit-year range.
  const auto max = Timestamp::FromEpochMilliseconds(253402300799000LL);
  EXPECT_EQ(max.Format(TimestampFormat::kDateTime), "9999-12-31T23:59:59Z");
  const auto parsed = Timestamp::Parse("9999-12-31T23:59:59Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, max);

  // 0001-01-01T00:00:00Z, the bottom.
  const auto min = Timestamp::Parse("0001-01-01T00:00:00Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(min.ok());
  EXPECT_EQ(min->Format(TimestampFormat::kDateTime), "0001-01-01T00:00:00Z");
}

TEST(TimestampDifferentialTest, HttpDateWeekdayMatchesGmtime) {
  static constexpr const char* kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  std::mt19937_64 rng(4242);
  std::uniform_int_distribution<std::int64_t> epochs(0, 4'000'000'000LL);
  for (int i = 0; i < 1000; ++i) {
    const std::int64_t seconds = epochs(rng);
    std::tm reference{};
    if (!GmTime(seconds, &reference)) {
      continue;
    }
    const auto ts = Timestamp::FromEpochMilliseconds(seconds * 1000);
    const std::string http_date = ts.Format(TimestampFormat::kHttpDate);
    ASSERT_EQ(http_date.substr(0, 3), kWeekdays[reference.tm_wday]) << http_date;
    const auto parsed = Timestamp::Parse(http_date, TimestampFormat::kHttpDate);
    ASSERT_TRUE(parsed.ok()) << http_date;
    ASSERT_EQ(parsed->epoch_milliseconds(), seconds * 1000) << http_date;
  }
}

TEST(TimestampDifferentialTest, EpochSecondsReconstructsFromItsOwnTextOverRandomInstants) {
  // The formatter the original issue #109 A-2 bug lived in: random
  // millisecond instants across the whole representable window, BOTH signs
  // and fractional parts included (the kDateTime sweep above samples whole
  // seconds only, which is exactly why it never caught the negative
  // fractional corruption). Two independent readers must both recover the
  // instant: the production parser (strtod-based) and a digit-by-digit
  // integer reconstruction that shares no code with either side.
  std::mt19937_64 rng(20260729);
  std::uniform_int_distribution<std::int64_t> instants(-62167219200000LL, 253402300799999LL);
  for (int i = 0; i < 2000; ++i) {
    const std::int64_t ms = instants(rng);
    const std::string text =
        Timestamp::FromEpochMilliseconds(ms).Format(TimestampFormat::kEpochSeconds);

    // Production parser round trip.
    const auto parsed = Timestamp::Parse(text, TimestampFormat::kEpochSeconds);
    ASSERT_TRUE(parsed.ok()) << text;
    ASSERT_EQ(parsed->epoch_milliseconds(), ms) << text;

    // Independent integer reconstruction: sign, whole seconds, then the
    // fraction re-padded to milliseconds. Also checks canonical form —
    // no trailing zeros, no bare trailing dot, fraction at most 3 digits.
    std::string_view rest = text;
    const bool negative = !rest.empty() && rest.front() == '-';
    if (negative) rest.remove_prefix(1);
    const std::size_t dot = rest.find('.');
    const std::string_view whole = rest.substr(0, dot);
    std::int64_t reconstructed = 0;
    for (const char c : whole) {
      ASSERT_TRUE(c >= '0' && c <= '9') << text;
      reconstructed = reconstructed * 10 + (c - '0');
    }
    reconstructed *= 1000;
    if (dot != std::string_view::npos) {
      const std::string_view fraction = rest.substr(dot + 1);
      ASSERT_FALSE(fraction.empty()) << text;   // no bare dot
      ASSERT_LE(fraction.size(), 3U) << text;   // millisecond precision
      ASSERT_NE(fraction.back(), '0') << text;  // canonical trim
      std::int64_t frac_ms = 0;
      for (const char c : fraction) {
        ASSERT_TRUE(c >= '0' && c <= '9') << text;
        frac_ms = frac_ms * 10 + (c - '0');
      }
      for (std::size_t pad = fraction.size(); pad < 3; ++pad) frac_ms *= 10;
      reconstructed += frac_ms;
    }
    if (negative) reconstructed = -reconstructed;
    ASSERT_EQ(reconstructed, ms) << text;
  }
}

}  // namespace
}  // namespace opal
