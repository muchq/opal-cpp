#ifndef SMITHY_CORE_TIMESTAMP_H_
#define SMITHY_CORE_TIMESTAMP_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "smithy/core/outcome.h"

namespace opal {

// The three timestamp serializations defined by the Smithy spec. Which one a
// member uses is decided by the protocol and the @timestampFormat trait.
enum class TimestampFormat {
  kEpochSeconds,  // 1515531081.123  (number, fractional seconds allowed)
  kDateTime,      // 1985-04-12T23:20:50.520Z  (RFC 3339)
  kHttpDate,      // Tue, 29 Apr 2014 18:30:38 GMT  (RFC 7231 IMF-fixdate)
};

// A point in time with millisecond precision, independent of time zone.
// Portable: no locale, environment, or platform time APIs are involved.
//
//   auto ts = Timestamp::Parse("2014-04-29T18:30:38Z", TimestampFormat::kDateTime);
//   std::string http_date = ts->Format(TimestampFormat::kHttpDate);
class Timestamp {
 public:
  Timestamp() = default;

  static Timestamp FromEpochMilliseconds(std::int64_t ms) { return Timestamp(ms); }
  // Rounds to the nearest millisecond.
  static Timestamp FromEpochSeconds(double seconds);

  // Range-checked factories for untrusted wire input. Serde uses these so a
  // hostile or malformed number cannot overflow the int64/double arithmetic
  // (undefined behavior) or land on an instant that formats to unparsable
  // text: they reject any value whose civil year falls outside the
  // RFC 3339 / IMF-fixdate representable window (0000-9999).
  static Outcome<Timestamp> FromEpochSecondsChecked(double seconds);
  static Outcome<Timestamp> FromEpochMillisecondsChecked(std::int64_t ms);

  std::int64_t epoch_milliseconds() const { return ms_; }
  double epoch_seconds() const { return static_cast<double>(ms_) / 1000.0; }

  // String formats are defined for years 0000-9999 (RFC 3339 / IMF-fixdate
  // four-digit years); instants outside that range produce unparsable text.
  std::string Format(TimestampFormat format) const;
  static Outcome<Timestamp> Parse(std::string_view text, TimestampFormat format);

  friend bool operator==(Timestamp a, Timestamp b) { return a.ms_ == b.ms_; }
  // The full comparison set, so timestamp-bearing generated structs can
  // default their own operator<=> (issue #49).
  friend std::strong_ordering operator<=>(Timestamp a, Timestamp b) { return a.ms_ <=> b.ms_; }

  // Debug rendering (smithy/core/print.h): RFC 3339, the most readable of
  // the three wire formats.
  void AppendDebugTo(std::string& out) const {
    out += "Timestamp(";
    out += Format(TimestampFormat::kDateTime);
    out += ')';
  }

 private:
  explicit Timestamp(std::int64_t ms) : ms_(ms) {}

  std::int64_t ms_ = 0;  // Milliseconds since 1970-01-01T00:00:00Z.
};

}  // namespace opal

// Hashes by the instant (epoch milliseconds), matching operator==, so
// timestamp-bearing generated structs can key unordered containers
// (issue #49). Process-local: never persist hash values.
template <>
struct std::hash<opal::Timestamp> {
  std::size_t operator()(opal::Timestamp timestamp) const noexcept {
    return std::hash<std::int64_t>{}(timestamp.epoch_milliseconds());
  }
};

#endif  // SMITHY_CORE_TIMESTAMP_H_
