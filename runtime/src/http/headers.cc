#include "opal/http/headers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <system_error>

namespace opal::http {
namespace {

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

namespace {

std::string_view TrimSpaces(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

}  // namespace

bool AcceptMatches(std::string_view accept_header, std::string_view content_type) {
  const auto expected_slash = content_type.find('/');
  const std::string_view expected_type = expected_slash == std::string_view::npos
                                             ? content_type
                                             : content_type.substr(0, expected_slash);
  while (!accept_header.empty()) {
    const auto comma = accept_header.find(',');
    std::string_view range = accept_header.substr(0, comma);
    if (const auto semi = range.find(';'); semi != std::string_view::npos) {
      range = range.substr(0, semi);
    }
    range = TrimSpaces(range);
    if (range == "*/*" || HeaderNameEquals(range, content_type)) return true;
    if (range.size() > 2 && range.substr(range.size() - 2) == "/*" &&
        HeaderNameEquals(range.substr(0, range.size() - 2), expected_type)) {
      return true;
    }
    if (comma == std::string_view::npos) break;
    accept_header.remove_prefix(comma + 1);
  }
  return false;
}

bool HeaderNameStartsWith(std::string_view name, std::string_view prefix) {
  return name.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), name.begin(),
                    [](char x, char y) { return AsciiLower(x) == AsciiLower(y); });
}

namespace {

constexpr std::array<std::string_view, 7> kAbbreviatedDays = {"Sun", "Mon", "Tue", "Wed",
                                                              "Thu", "Fri", "Sat"};
constexpr std::array<std::string_view, 7> kFullDays = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                       "Thursday", "Friday", "Saturday"};

bool AllDigits(std::string_view text) {
  return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
}

// The whole of `text` as an int, or nullopt. from_chars rather than stoi:
// this library builds under -fno-exceptions too, and a conversion that
// reports failure in its return value has nothing to throw.
std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// The reference's civil year, via the format the runtime already tests, so no
// second civil decomposition exists to disagree with the first. Negative when
// the instant falls outside the representable window, where a two-digit year
// cannot be resolved against anything.
int ReferenceYear(Timestamp reference) {
  const std::string rendered = reference.Format(TimestampFormat::kDateTime);
  if (rendered.size() < 4) {
    return -1;
  }
  // AllDigits before the conversion because from_chars would take a leading
  // '-', and a negative year here is garbage rather than an instant.
  const std::string_view year = std::string_view(rendered).substr(0, 4);
  return AllDigits(year) ? ParseInt(year).value_or(-1) : -1;
}

// RFC 9110 §5.6.7: a two-digit year more than fifty years ahead of the
// reference is the most recent past year ending in those digits. Without it a
// 1994 timestamp reads as 2094, and a delay in the past becomes seventy years
// in the future.
std::string ResolveTwoDigitYear(int two_digits, int reference_year) {
  const int century = (reference_year / 100) * 100;
  int year = century + two_digits;
  if (year > reference_year + 50) {
    year -= 100;
  }
  std::string text = std::to_string(year);
  return std::string(4 - text.size(), '0') + text;
}

// "Sunday, 06-Nov-94 08:49:37 GMT" as IMF-fixdate, or empty when it is not
// that shape. The weekday is carried across rather than recomputed, so the
// IMF-fixdate parser's own weekday check still has something to check.
std::string Rfc850AsFixdate(std::string_view text, Timestamp reference) {
  const auto comma = text.find(',');
  if (comma == std::string_view::npos) return {};
  std::size_t day = 0;
  while (day < kFullDays.size() && kFullDays[day] != text.substr(0, comma)) ++day;
  if (day == kFullDays.size()) return {};

  // " 06-Nov-94 08:49:37 GMT" is fixed-width once the day name is off.
  const std::string_view rest = text.substr(comma + 1);
  if (rest.size() != 23 || rest[0] != ' ' || rest[3] != '-' || rest[7] != '-' || rest[10] != ' ' ||
      rest.substr(19) != " GMT") {
    return {};
  }
  const std::string_view day_of_month = rest.substr(1, 2);
  const std::string_view month = rest.substr(4, 3);
  const std::string_view two_digit_year = rest.substr(8, 2);
  const std::string_view time_of_day = rest.substr(11, 8);
  if (!AllDigits(day_of_month) || !AllDigits(two_digit_year)) return {};
  const int reference_year = ReferenceYear(reference);
  const auto year_digits = ParseInt(two_digit_year);
  if (reference_year < 0 || !year_digits.has_value()) return {};

  return std::string(kAbbreviatedDays[day]) + ", " + std::string(day_of_month) + " " +
         std::string(month) + " " + ResolveTwoDigitYear(*year_digits, reference_year) + " " +
         std::string(time_of_day) + " GMT";
}

// "Sun Nov  6 08:49:37 1994" as IMF-fixdate, or empty. Fixed-width, with the
// day of the month space-padded rather than zero-padded.
std::string AsctimeAsFixdate(std::string_view text) {
  if (text.size() != 24 || text[3] != ' ' || text[7] != ' ' || text[10] != ' ' || text[19] != ' ') {
    return {};
  }
  std::size_t day = 0;
  while (day < kAbbreviatedDays.size() && kAbbreviatedDays[day] != text.substr(0, 3)) ++day;
  if (day == kAbbreviatedDays.size()) return {};

  const std::string_view month = text.substr(4, 3);
  const std::string_view time_of_day = text.substr(11, 8);
  const std::string_view year = text.substr(20, 4);
  const std::string day_of_month =
      text[8] == ' ' ? "0" + std::string(text.substr(9, 1)) : std::string(text.substr(8, 2));
  if (!AllDigits(day_of_month) || !AllDigits(year)) return {};

  return std::string(kAbbreviatedDays[day]) + ", " + day_of_month + " " + std::string(month) + " " +
         std::string(year) + " " + std::string(time_of_day) + " GMT";
}

}  // namespace

std::optional<Timestamp> ParseHttpDate(std::string_view text, Timestamp reference) {
  // IMF-fixdate first: the only form a sender is allowed to produce, and so
  // the only one worth trying before the two kept alive for old peers.
  if (const auto fixdate = Timestamp::Parse(text, TimestampFormat::kHttpDate); fixdate.ok()) {
    return *fixdate;
  }
  std::string normalized = Rfc850AsFixdate(text, reference);
  if (normalized.empty()) {
    normalized = AsctimeAsFixdate(text);
  }
  if (normalized.empty()) {
    return std::nullopt;
  }
  const auto parsed = Timestamp::Parse(normalized, TimestampFormat::kHttpDate);
  if (!parsed.ok()) {
    return std::nullopt;
  }
  return *parsed;
}

bool HeaderNameEquals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return AsciiLower(x) == AsciiLower(y);
         });
}

std::optional<std::string> Headers::Get(std::string_view name) const {
  for (const auto& [key, value] : entries_) {
    if (HeaderNameEquals(key, name)) return value;
  }
  return std::nullopt;
}

std::vector<std::string> Headers::GetAll(std::string_view name) const {
  std::vector<std::string> values;
  for (const auto& [key, value] : entries_) {
    if (HeaderNameEquals(key, name)) values.push_back(value);
  }
  return values;
}

bool Headers::Has(std::string_view name) const { return Get(name).has_value(); }

void Headers::Set(std::string_view name, std::string_view value) {
  Remove(name);
  Add(name, value);
}

void Headers::Add(std::string_view name, std::string_view value) {
  entries_.emplace_back(std::string(name), std::string(value));
}

void Headers::Remove(std::string_view name) {
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&](const auto& entry) { return HeaderNameEquals(entry.first, name); }),
      entries_.end());
}

std::vector<std::string> SplitHeaderListValues(std::string_view value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    std::size_t comma = value.find(',', start);
    std::string_view part =
        value.substr(start, comma == std::string_view::npos ? comma : comma - start);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
    out.emplace_back(part);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string MediaTypeOf(std::string_view content_type) {
  std::string_view media = content_type.substr(0, content_type.find(';'));
  while (!media.empty() && (media.front() == ' ' || media.front() == '\t')) {
    media.remove_prefix(1);
  }
  while (!media.empty() && (media.back() == ' ' || media.back() == '\t')) {
    media.remove_suffix(1);
  }
  std::string out(media);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool ValidHeaderName(std::string_view name) {
  // Control bytes split or corrupt the field line; space and colon corrupt
  // the name/value boundary. (Full token validation would also exclude
  // separators like '(' — the defense here is scoped to what breaks the
  // wire, not to RFC pedantry.)
  return !name.empty() && std::ranges::none_of(name, [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte <= 0x20 || byte == 0x7F || c == ':';
  });
}

bool ValidHeaderValue(std::string_view value) {
  return std::ranges::all_of(value, [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    // HTAB is legal inside field values; every other control byte and DEL
    // is not.
    return byte == '\t' || (byte >= 0x20 && byte != 0x7F);
  });
}

std::optional<std::string> FindUnsafeHeader(const Headers& headers) {
  for (const auto& [name, value] : headers.entries()) {
    if (!ValidHeaderName(name) || !ValidHeaderValue(value)) return name;
  }
  return std::nullopt;
}

bool ValidRequestLineField(std::string_view field) {
  return std::ranges::none_of(field, [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    // Space and every control byte split or corrupt the request line; DEL
    // too. Colon is allowed (unlike a header name) — it is legal in a
    // target's path and query.
    return byte <= 0x20 || byte == 0x7F;
  });
}

std::vector<std::string> SplitHttpDateHeaderValues(std::string_view value) {
  std::vector<std::string> tokens = SplitHeaderListValues(value);
  std::vector<std::string> out;
  for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
    out.push_back(tokens[i] + ", " + tokens[i + 1]);
  }
  if (tokens.size() % 2 != 0 && !tokens.back().empty()) {
    out.push_back(tokens.back());
  }
  return out;
}

}  // namespace opal::http
