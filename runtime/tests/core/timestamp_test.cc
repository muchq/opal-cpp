#include "smithy/core/timestamp.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace opal {
namespace {

TEST(TimestampOrderingTest, ProvidesTheFullComparisonSet) {
  // operator<=> (not just the old ==/<) so timestamp-bearing generated
  // structs can default their own <=> (issue #49).
  const Timestamp early = Timestamp::FromEpochSeconds(1);
  const Timestamp late = Timestamp::FromEpochSeconds(2);
  EXPECT_TRUE(early < late);
  EXPECT_TRUE(late > early);
  EXPECT_TRUE(early <= Timestamp::FromEpochSeconds(1));
  EXPECT_TRUE(early >= Timestamp::FromEpochSeconds(1));
}

TEST(TimestampParseTest, EpochSecondsIsStrict) {
  using opal::Timestamp;
  using opal::TimestampFormat;
  EXPECT_TRUE(Timestamp::Parse("1515531081", TimestampFormat::kEpochSeconds).ok());
  EXPECT_TRUE(Timestamp::Parse("1515531081.123", TimestampFormat::kEpochSeconds).ok());
  EXPECT_TRUE(Timestamp::Parse("-5", TimestampFormat::kEpochSeconds).ok());
  for (const char* bad :
       {"0x42", "1e3", "Infinity", "NaN", "true", "1515531081ABC", "1.", ".5", "-", "1.2.3", ""}) {
    EXPECT_FALSE(Timestamp::Parse(bad, TimestampFormat::kEpochSeconds).ok()) << bad;
  }
}

TEST(TimestampCheckedTest, AcceptsInRangeInstants) {
  const auto a = Timestamp::FromEpochSecondsChecked(1515531081.123);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->epoch_milliseconds(), 1515531081123);
  EXPECT_TRUE(Timestamp::FromEpochSecondsChecked(0.0).ok());
  EXPECT_TRUE(Timestamp::FromEpochSecondsChecked(-5.0).ok());  // pre-epoch
  EXPECT_TRUE(Timestamp::FromEpochMillisecondsChecked(0).ok());
}

TEST(TimestampCheckedTest, AcceptsTheRepresentableBoundaries) {
  // The exact edges of the RFC 3339 window round-trip.
  const auto max_dt = Timestamp::Parse("9999-12-31T23:59:59.999Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(max_dt.ok());
  EXPECT_TRUE(Timestamp::FromEpochMillisecondsChecked(max_dt->epoch_milliseconds()).ok());
  const auto min_dt = Timestamp::Parse("0000-01-01T00:00:00Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(min_dt.ok());
  EXPECT_TRUE(Timestamp::FromEpochMillisecondsChecked(min_dt->epoch_milliseconds()).ok());
}

TEST(TimestampCheckedTest, RejectsOutOfRangeAndNonFinite) {
  // The values the CBOR/JSON UB findings are about: a huge integer scaled to
  // milliseconds, an out-of-range float, and non-finite doubles.
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(1e300).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(-1e300).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(static_cast<double>(9223372036854775LL)).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(std::numeric_limits<double>::infinity()).ok());
  EXPECT_FALSE(Timestamp::FromEpochSecondsChecked(std::nan("")).ok());
  // Just past year 9999 / before year 0000.
  const auto past_max = Timestamp::Parse("9999-12-31T23:59:59.999Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(past_max.ok());
  EXPECT_FALSE(Timestamp::FromEpochMillisecondsChecked(past_max->epoch_milliseconds() + 1).ok());
}

TEST(TimestampTest, FormatsDateTime) {
  // 1985-04-12T23:20:50.520Z, the Smithy spec's canonical example.
  const auto ts = Timestamp::FromEpochMilliseconds(482196050520);
  EXPECT_EQ(ts.Format(TimestampFormat::kDateTime), "1985-04-12T23:20:50.52Z");
}

TEST(TimestampTest, FormatsDateTimeWithoutFraction) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238000);
  EXPECT_EQ(ts.Format(TimestampFormat::kDateTime), "2014-04-29T18:30:38Z");
}

TEST(TimestampTest, ParsesDateTime) {
  const auto ts = Timestamp::Parse("1985-04-12T23:20:50.52Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok()) << ts.error().message();
  EXPECT_EQ(ts->epoch_milliseconds(), 482196050520);
}

TEST(TimestampTest, ParsesDateTimeWithOffset) {
  const auto with_offset =
      Timestamp::Parse("2014-04-29T20:30:38+02:00", TimestampFormat::kDateTime);
  ASSERT_TRUE(with_offset.ok());
  EXPECT_EQ(with_offset->epoch_milliseconds(), 1398796238000);
}

TEST(TimestampTest, ParsesLowercaseSeparators) {
  const auto ts = Timestamp::Parse("2014-04-29t18:30:38z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts->epoch_milliseconds(), 1398796238000);
}

TEST(TimestampTest, TruncatesSubMillisecondDigits) {
  const auto ts = Timestamp::Parse("1985-04-12T23:20:50.52099Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts->epoch_milliseconds(), 482196050520);
}

TEST(TimestampTest, RejectsInvalidDateTime) {
  const char* bad[] = {
      "1985-04-12",                        // date only
      "1985-04-12T23:20:50",               // missing offset
      "1985-04-31T23:20:50Z",              // April has 30 days
      "1985-02-29T00:00:00Z",              // not a leap year
      "1985-13-12T23:20:50Z",              // month 13
      "1985-04-12T24:00:00Z",              // hour 24
      "1985-04-12T23:20:60Z",              // leap second unsupported
      "1985-04-12T23:20:50.Z",             // empty fraction
      "1985-04-12T23:20:50Zjunk",          // trailing junk
      "1985-04-12T23:20:50+2:00",          // malformed offset
      "trailer 1985-04-12T23:20:50.52Z 1"  // garbage
  };
  for (const char* text : bad) {
    EXPECT_FALSE(Timestamp::Parse(text, TimestampFormat::kDateTime).ok()) << text;
  }
}

TEST(TimestampTest, LeapDayRoundTrips) {
  const auto ts = Timestamp::Parse("2024-02-29T12:00:00Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_EQ(ts->Format(TimestampFormat::kDateTime), "2024-02-29T12:00:00Z");
}

TEST(TimestampTest, PreEpochRoundTrips) {
  const auto ts = Timestamp::Parse("1969-07-20T20:17:40Z", TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.ok());
  EXPECT_LT(ts->epoch_milliseconds(), 0);
  EXPECT_EQ(ts->Format(TimestampFormat::kDateTime), "1969-07-20T20:17:40Z");
  EXPECT_EQ(ts->Format(TimestampFormat::kHttpDate), "Sun, 20 Jul 1969 20:17:40 GMT");
}

TEST(TimestampTest, FormatsNegativeFractionalEpochSeconds) {
  // The issue #109 A-2 class: floor-division rendered −500 ms as "-1.5",
  // which Parse (correctly) read back as −1500 ms — every pre-1970 instant
  // with a nonzero millisecond part was wire-corrupted. Sign and magnitude
  // are now formatted separately.
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(-500).Format(TimestampFormat::kEpochSeconds), "-0.5");
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(-1).Format(TimestampFormat::kEpochSeconds), "-0.001");
  // A boundary pin, not a corruption pin: whole-second negatives were
  // rendered correctly by the old code too.
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(-1000).Format(TimestampFormat::kEpochSeconds), "-1");
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(-1500).Format(TimestampFormat::kEpochSeconds), "-1.5");
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(-999).Format(TimestampFormat::kEpochSeconds),
            "-0.999");
  // Positives and zero are unchanged by the rewrite.
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(0).Format(TimestampFormat::kEpochSeconds), "0");
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(500).Format(TimestampFormat::kEpochSeconds), "0.5");
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(1515531081123).Format(TimestampFormat::kEpochSeconds),
            "1515531081.123");
}

TEST(TimestampTest, EpochSecondsBoundariesFormatToCanonicalText) {
  // Exact strings, not just round-trip identity — identity alone cannot
  // catch a trailing-zero-trim regression (e.g. "-62167219199.9990" reads
  // back to the same instant).
  EXPECT_EQ(
      Timestamp::FromEpochMilliseconds(-62167219200000).Format(TimestampFormat::kEpochSeconds),
      "-62167219200");
  EXPECT_EQ(
      Timestamp::FromEpochMilliseconds(-62167219199999).Format(TimestampFormat::kEpochSeconds),
      "-62167219199.999");
}

TEST(TimestampTest, EpochSecondsIsTotalOverTheUncheckedInt64Domain) {
  // The unchecked factory can mint any int64. The epoch-seconds arm must
  // format the extremes without UB — the civil decomposition, whose day
  // arithmetic cannot survive them, runs only in the arms that need it.
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(std::numeric_limits<std::int64_t>::min())
                .Format(TimestampFormat::kEpochSeconds),
            "-9223372036854775.808");
  EXPECT_EQ(Timestamp::FromEpochMilliseconds(std::numeric_limits<std::int64_t>::max())
                .Format(TimestampFormat::kEpochSeconds),
            "9223372036854775.807");
}

TEST(TimestampTest, NegativeEpochSecondsRoundTripThroughTheirOwnText) {
  // Parse ∘ Format must be the identity — the property the old rendering
  // broke. Sweep sign/fraction boundaries down to the year-0000 floor.
  constexpr std::int64_t kMinRepresentableMs = -62167219200000;
  for (const std::int64_t ms :
       {std::int64_t{-1}, std::int64_t{-500}, std::int64_t{-999}, std::int64_t{-1000},
        std::int64_t{-1001}, std::int64_t{-1500}, std::int64_t{-86400001},
        std::int64_t{-14159025000}, kMinRepresentableMs, kMinRepresentableMs + 1, std::int64_t{0},
        std::int64_t{1}, std::int64_t{500}, std::int64_t{1515531081123}}) {
    const std::string text =
        Timestamp::FromEpochMilliseconds(ms).Format(TimestampFormat::kEpochSeconds);
    const auto parsed = Timestamp::Parse(text, TimestampFormat::kEpochSeconds);
    ASSERT_TRUE(parsed.ok()) << text;
    EXPECT_EQ(parsed->epoch_milliseconds(), ms) << text;
  }
}

TEST(TimestampTest, HttpDateNamesEveryWeekdayAndMonth) {
  // Pins the weekday/month tables end to end (they now feed %s as
  // const char*): seven consecutive days cover the weekday cycle...
  const std::array<std::string, 7> weekdays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int day = 0; day < 7; ++day) {
    // 2023-01-01 was a Sunday.
    const auto ts = Timestamp::FromEpochMilliseconds((std::int64_t{19358} + day) * 86400000);
    const std::string text = ts.Format(TimestampFormat::kHttpDate);
    EXPECT_EQ(text.substr(0, 3), weekdays[static_cast<std::size_t>(day)]) << text;
    const auto parsed = Timestamp::Parse(text, TimestampFormat::kHttpDate);
    ASSERT_TRUE(parsed.ok()) << text;
    EXPECT_EQ(*parsed, ts);
  }
  // ...and the 15th of each 2023 month covers the month table.
  const std::array<std::string, 12> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int month = 1; month <= 12; ++month) {
    char text[32];
    std::snprintf(text, sizeof text, "2023-%02d-15T12:00:00Z", month);
    const auto ts = Timestamp::Parse(text, TimestampFormat::kDateTime);
    ASSERT_TRUE(ts.ok()) << text;
    const std::string http = ts->Format(TimestampFormat::kHttpDate);
    EXPECT_EQ(http.substr(8, 3), months[static_cast<std::size_t>(month - 1)]) << http;
    const auto parsed = Timestamp::Parse(http, TimestampFormat::kHttpDate);
    ASSERT_TRUE(parsed.ok()) << http;
    EXPECT_EQ(*parsed, *ts);
  }
}

TEST(TimestampTest, FormatsAndParsesHttpDate) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238000);
  const std::string text = ts.Format(TimestampFormat::kHttpDate);
  EXPECT_EQ(text, "Tue, 29 Apr 2014 18:30:38 GMT");
  const auto parsed = Timestamp::Parse(text, TimestampFormat::kHttpDate);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, ts);
}

TEST(TimestampTest, RejectsInvalidHttpDate) {
  const char* bad[] = {"Wed, 29 Apr 2014 18:30:38 GMT",   // 2014-04-29 was a Tuesday
                       "Tue, 29 Apr 2014 18:30:38 UTC",   // zone must be GMT
                       "Tue, 29 Abc 2014 18:30:38 GMT",   // bad month
                       "Tuesday, 29 Apr 2014 18:30 GMT",  // not IMF-fixdate
                       ""};
  for (const char* text : bad) {
    EXPECT_FALSE(Timestamp::Parse(text, TimestampFormat::kHttpDate).ok()) << text;
  }
}

TEST(TimestampTest, EpochSecondsRoundTrips) {
  const auto ts = Timestamp::FromEpochMilliseconds(1515531081123);
  EXPECT_EQ(ts.Format(TimestampFormat::kEpochSeconds), "1515531081.123");
  const auto parsed = Timestamp::Parse("1515531081.123", TimestampFormat::kEpochSeconds);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, ts);
}

TEST(TimestampTest, EpochSecondsIntegral) {
  const auto ts = Timestamp::FromEpochMilliseconds(1515531081000);
  EXPECT_EQ(ts.Format(TimestampFormat::kEpochSeconds), "1515531081");
}

TEST(TimestampTest, EpochSecondsPreEpochFormat) {
  // -0.5s => floor(seconds) = -1 with fraction .5 => "-0.5".
  const auto ts = Timestamp::FromEpochMilliseconds(-500);
  const auto parsed = Timestamp::Parse("-0.5", TimestampFormat::kEpochSeconds);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(*parsed, ts);
}

TEST(TimestampTest, RejectsInvalidEpochSeconds) {
  const char* bad[] = {"", "abc", "1.2.3", "1e999", "nan", "inf"};
  for (const char* text : bad) {
    EXPECT_FALSE(Timestamp::Parse(text, TimestampFormat::kEpochSeconds).ok()) << text;
  }
}

}  // namespace
}  // namespace opal
