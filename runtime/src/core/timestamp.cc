#include "opal/core/timestamp.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Calendar math uses the C++20 <chrono> civil calendar types (year_month_day,
// sys_days, weekday), which are constexpr, timezone-free, and supported at our
// compiler floor. String formatting/parsing stays hand-rolled: std::format and
// std::chrono::parse for chrono types are not available across gcc 11 / clang 14
// / MSVC 19.30.

namespace opal {
namespace {

constexpr std::int64_t kMsPerSecond = 1000;
constexpr std::int64_t kMsPerDay = 86400 * kMsPerSecond;

// const char*, not string_view: these feed snprintf's %s, which needs the
// NUL a literal-backed view only happens to have (issue #109, SL.str). The
// parse-side comparisons below still work — string_view compares against
// const char* by value.
constexpr std::array<const char*, 7> kWeekdays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct CivilTime {
  int year = 1970;
  unsigned month = 1;  // 1-12
  unsigned day = 1;    // 1-31
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;

  // False for out-of-range fields, including day-of-month vs. leap years.
  bool ok() const {
    return ymd().ok() && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 &&
           second <= 59;
  }

  std::chrono::year_month_day ymd() const {
    return std::chrono::year{year} / static_cast<int>(month) / static_cast<int>(day);
  }
};

std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
  return a / b - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
}

std::int64_t EpochDays(const CivilTime& c) {
  return std::chrono::sys_days(c.ymd()).time_since_epoch().count();
}

CivilTime Decompose(std::int64_t ms) {
  const std::int64_t days = FloorDiv(ms, kMsPerDay);
  std::int64_t ms_of_day = ms - days * kMsPerDay;

  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
  CivilTime civil;
  civil.year = static_cast<int>(ymd.year());
  civil.month = static_cast<unsigned>(ymd.month());
  civil.day = static_cast<unsigned>(ymd.day());
  civil.hour = static_cast<int>(ms_of_day / (3600 * kMsPerSecond));
  ms_of_day %= 3600 * kMsPerSecond;
  civil.minute = static_cast<int>(ms_of_day / (60 * kMsPerSecond));
  ms_of_day %= 60 * kMsPerSecond;
  civil.second = static_cast<int>(ms_of_day / kMsPerSecond);
  civil.millisecond = static_cast<int>(ms_of_day % kMsPerSecond);
  return civil;
}

std::int64_t Compose(const CivilTime& c) {
  return EpochDays(c) * kMsPerDay +
         (static_cast<std::int64_t>(c.hour) * 3600 + static_cast<std::int64_t>(c.minute) * 60 +
          c.second) *
             kMsPerSecond +
         c.millisecond;
}

std::size_t WeekdayIndex(std::int64_t epoch_days) {
  const std::chrono::weekday weekday{std::chrono::sys_days{std::chrono::days{epoch_days}}};
  return weekday.c_encoding();  // 0 = Sunday, matching kWeekdays.
}

// Appends ".<fraction>" with trailing zeros trimmed, or nothing when ms == 0.
void AppendFraction(std::string* out, int ms) {
  if (ms == 0) return;
  std::array<char, 8> buffer{};
  std::snprintf(buffer.data(), buffer.size(), ".%03d", ms);
  std::string_view fraction(buffer.data());
  while (fraction.ends_with('0')) fraction.remove_suffix(1);
  out->append(fraction);
}

bool ParseDigits(std::string_view text, std::size_t pos, std::size_t count, int* out) {
  if (pos + count > text.size()) return false;
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[pos + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

bool ParseDigits(std::string_view text, std::size_t pos, std::size_t count, unsigned* out) {
  int value = 0;
  if (!ParseDigits(text, pos, count, &value)) return false;
  *out = static_cast<unsigned>(value);
  return true;
}

Outcome<Timestamp> ParseDateTime(std::string_view text) {
  const auto invalid = [&] {
    return Error::Serialization("timestamp: invalid date-time: " + std::string(text));
  };
  CivilTime c;
  if (!ParseDigits(text, 0, 4, &c.year) || text.size() < 20 || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':') {
    return invalid();
  }
  if (!ParseDigits(text, 5, 2, &c.month) || !ParseDigits(text, 8, 2, &c.day) ||
      !ParseDigits(text, 11, 2, &c.hour) || !ParseDigits(text, 14, 2, &c.minute) ||
      !ParseDigits(text, 17, 2, &c.second)) {
    return invalid();
  }
  std::size_t pos = 19;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    const std::size_t fraction_start = pos;
    int scale = 100;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (pos - fraction_start < 3) {
        c.millisecond += (text[pos] - '0') * scale;
        scale /= 10;
      }
      ++pos;
    }
    if (pos == fraction_start) return invalid();
  }
  // Offset: 'Z' or ±hh:mm.
  std::int64_t offset_ms = 0;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
    ++pos;
  } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const bool negative = text[pos] == '-';
    int oh = 0;
    int om = 0;
    if (!ParseDigits(text, pos + 1, 2, &oh) || pos + 3 >= text.size() || text[pos + 3] != ':' ||
        !ParseDigits(text, pos + 4, 2, &om) || oh > 23 || om > 59) {
      return invalid();
    }
    offset_ms =
        (static_cast<std::int64_t>(oh) * 3600 + static_cast<std::int64_t>(om) * 60) * kMsPerSecond;
    if (negative) offset_ms = -offset_ms;
    pos += 6;
  } else {
    return invalid();
  }
  if (pos != text.size() || !c.ok()) return invalid();
  return Timestamp::FromEpochMilliseconds(Compose(c) - offset_ms);
}

Outcome<Timestamp> ParseHttpDate(std::string_view text) {
  const auto invalid = [&] {
    return Error::Serialization("timestamp: invalid http-date: " + std::string(text));
  };
  // IMF-fixdate is fixed-width: "Sun, 06 Nov 1994 08:49:37 GMT" (29 chars).
  if (text.size() != 29 || text.substr(3, 2) != ", " || text[7] != ' ' || text[11] != ' ' ||
      text[16] != ' ' || text[19] != ':' || text[22] != ':' || text.substr(25) != " GMT") {
    return invalid();
  }
  CivilTime c;
  if (!ParseDigits(text, 5, 2, &c.day) || !ParseDigits(text, 12, 4, &c.year) ||
      !ParseDigits(text, 17, 2, &c.hour) || !ParseDigits(text, 20, 2, &c.minute) ||
      !ParseDigits(text, 23, 2, &c.second)) {
    return invalid();
  }
  c.month = 0;
  const std::string_view month_text = text.substr(8, 3);
  for (std::size_t i = 0; i < kMonths.size(); ++i) {
    if (kMonths[i] == month_text) c.month = static_cast<unsigned>(i) + 1;
  }
  if (c.month == 0 || !c.ok()) return invalid();
  if (kWeekdays[WeekdayIndex(EpochDays(c))] != text.substr(0, 3)) return invalid();
  return Timestamp::FromEpochMilliseconds(Compose(c));
}

Outcome<Timestamp> ParseEpochSeconds(std::string_view text) {
  // Strict grammar: [-]digits[.digits] — no hex, exponents, or infinities
  // (the malformed-request suites reject "0x42", "1e3", "Infinity", ...).
  const auto invalid = [&] {
    return Error::Serialization("timestamp: invalid epoch-seconds: " + std::string(text));
  };
  std::string_view rest = text;
  if (!rest.empty() && rest.front() == '-') rest.remove_prefix(1);
  if (rest.empty()) return invalid();
  bool seen_dot = false;
  bool digits_before_dot = false;
  bool digits_after_dot = false;
  for (const char c : rest) {
    if (c >= '0' && c <= '9') {
      (seen_dot ? digits_after_dot : digits_before_dot) = true;
    } else if (c == '.' && !seen_dot) {
      seen_dot = true;
    } else {
      return invalid();
    }
  }
  if (!digits_before_dot || (seen_dot && !digits_after_dot)) return invalid();
  const std::string buffer(text);
  errno = 0;
  const double seconds = std::strtod(buffer.c_str(), nullptr);
  if (errno == ERANGE || !std::isfinite(seconds)) return invalid();
  return Timestamp::FromEpochSecondsChecked(seconds);
}

// Epoch-milliseconds bounds of the RFC 3339 / IMF-fixdate representable window
// (0000-01-01T00:00:00.000Z .. 9999-12-31T23:59:59.999Z). Instants outside it
// both overflow the arithmetic below and format to text no conformant peer can
// parse, so untrusted numbers beyond it are rejected rather than corrupted.
constexpr std::int64_t kMinRepresentableMs = -62167219200000;  // year 0000-01-01
constexpr std::int64_t kMaxRepresentableMs = 253402300799999;  // year 9999-12-31T23:59:59.999

Outcome<Timestamp> CheckedFromMs(std::int64_t ms) {
  if (ms < kMinRepresentableMs || ms > kMaxRepresentableMs) {
    return Error::Serialization("timestamp: instant out of representable range (year 0000-9999)");
  }
  return Timestamp::FromEpochMilliseconds(ms);
}

}  // namespace

Timestamp Timestamp::FromEpochSeconds(double seconds) {
  return Timestamp(static_cast<std::int64_t>(std::llround(seconds * 1000.0)));
}

Outcome<Timestamp> Timestamp::FromEpochSecondsChecked(double seconds) {
  if (!std::isfinite(seconds)) {
    return Error::Serialization("timestamp: epoch-seconds is not finite");
  }
  // Bound the value before scaling so neither `seconds * 1000` nor the cast to
  // int64 can overflow; CheckedFromMs then applies the exact year window.
  constexpr double kMaxSeconds = 253402300800.0;  // just past year 9999
  constexpr double kMinSeconds = -62167219200.0;  // year 0000-01-01
  if (seconds < kMinSeconds || seconds > kMaxSeconds) {
    return Error::Serialization(
        "timestamp: epoch-seconds out of representable range (year 0000-9999)");
  }
  return CheckedFromMs(static_cast<std::int64_t>(std::llround(seconds * 1000.0)));
}

Outcome<Timestamp> Timestamp::FromEpochMillisecondsChecked(std::int64_t ms) {
  return CheckedFromMs(ms);
}

std::string Timestamp::Format(TimestampFormat format) const {
  std::array<char, 40> buffer{};
  std::string out;
  switch (format) {
    case TimestampFormat::kEpochSeconds: {
      // Sign first, then |ms_| decomposed. Floor-division rendered −0.5 s
      // as whole −1 plus the positive fraction .5 — "-1.5", which Parse
      // (correctly) read back as −1500 ms: every pre-1970 instant with a
      // nonzero millisecond part was wire-corrupted (issue #109). Unsigned
      // negation, not std::abs: the unchecked factory can mint INT64_MIN,
      // whose two's-complement abs is UB. Decompose runs only in the arms
      // that need civil fields — its day arithmetic cannot survive the
      // int64 extremes — so this arm is total over the unchecked domain.
      const bool negative = ms_ < 0;
      const auto magnitude =
          negative ? 0 - static_cast<std::uint64_t>(ms_) : static_cast<std::uint64_t>(ms_);
      if (negative) out.push_back('-');
      out += std::to_string(magnitude / static_cast<std::uint64_t>(kMsPerSecond));
      AppendFraction(&out, static_cast<int>(magnitude % static_cast<std::uint64_t>(kMsPerSecond)));
      return out;
    }
    case TimestampFormat::kDateTime: {
      const CivilTime c = Decompose(ms_);
      std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02uT%02d:%02d:%02d", c.year, c.month,
                    c.day, c.hour, c.minute, c.second);
      out = buffer.data();
      AppendFraction(&out, c.millisecond);
      out.push_back('Z');
      return out;
    }
    case TimestampFormat::kHttpDate: {
      const CivilTime c = Decompose(ms_);
      const std::size_t weekday = WeekdayIndex(FloorDiv(ms_, kMsPerDay));
      std::snprintf(buffer.data(), buffer.size(), "%s, %02u %s %04d %02d:%02d:%02d GMT",
                    kWeekdays[weekday], c.day, kMonths[c.month - 1], c.year, c.hour, c.minute,
                    c.second);
      return buffer.data();
    }
  }
  return out;  // Unreachable with a valid format.
}

Outcome<Timestamp> Timestamp::Parse(std::string_view text, TimestampFormat format) {
  switch (format) {
    case TimestampFormat::kEpochSeconds:
      return ParseEpochSeconds(text);
    case TimestampFormat::kDateTime:
      return ParseDateTime(text);
    case TimestampFormat::kHttpDate:
      return ParseHttpDate(text);
  }
  return Error::Serialization("timestamp: unknown format");
}

}  // namespace opal
