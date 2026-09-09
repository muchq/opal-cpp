#include "opal/http/headers.h"

#include <algorithm>
#include <cctype>

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
