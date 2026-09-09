#ifndef SMITHY_CORE_PRINT_H_
#define SMITHY_CORE_PRINT_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "smithy/core/container_traits.h"

namespace opal {

// Debug printing for generated types (issue #85): every generated type
// carries `AppendDebugTo(std::string&)` (the one primitive), with
// `DebugString()` and `operator<<` as thin adapters over it. DebugAppend
// renders the member types generated code aggregates; a type with an
// AppendDebugTo member prints through it.
//
// Debug output is for humans and logs. It is NOT a serialization format:
// never parse it, and never pin exact output across library versions —
// protobuf's DebugString became load-bearing exactly this way (issue #85).

namespace internal {

// C-escaped, double-quoted string rendering.
inline void AppendQuoted(std::string& out, std::string_view text) {
  out += '"';
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::array<char, 8> buffer{};
          std::snprintf(buffer.data(), buffer.size(), "\\x%02x", static_cast<unsigned char>(c));
          out += buffer.data();
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// Shortest round-trip rendering for integers and floating point.
template <typename T>
void AppendArithmetic(std::string& out, T value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  out.append(buffer.data(), result.ptr);
}

}  // namespace internal

// Appends the debug rendering of `value`: AppendDebugTo members win, strings
// quote and escape, containers render element-wise ([a, b] / {"k": v}),
// engaged optionals render their value (disengaged: `nullopt`), enums render
// their underlying number, and arithmetic renders shortest-round-trip.
template <typename T>
void DebugAppend(std::string& out, const T& value) {
  if constexpr (requires { value.AppendDebugTo(out); }) {
    value.AppendDebugTo(out);
  } else if constexpr (std::is_same_v<T, bool>) {
    out += value ? "true" : "false";
  } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    internal::AppendQuoted(out, std::string_view(value));
  } else if constexpr (std::is_enum_v<T>) {
    internal::AppendArithmetic(out, static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    internal::AppendArithmetic(out, value);
  } else if constexpr (internal::IsVector<T>::value) {
    out += '[';
    const char* sep = "";
    // Bind as value_type: vector<bool> iteration yields a proxy (see the
    // matching note in hash.h — caught by libc++ on PR #84).
    for (const typename T::value_type& element : value) {
      out += sep;
      sep = ", ";
      DebugAppend(out, element);
    }
    out += ']';
  } else if constexpr (internal::IsMap<T>::value) {
    out += '{';
    const char* sep = "";
    for (const auto& [key, mapped] : value) {
      out += sep;
      sep = ", ";
      DebugAppend(out, key);
      out += ": ";
      DebugAppend(out, mapped);
    }
    out += '}';
  } else if constexpr (internal::IsOptional<T>::value) {
    if (value.has_value()) {
      DebugAppend(out, *value);
    } else {
      out += "nullopt";
    }
  } else {
    static_assert(!sizeof(T*), "no debug rendering for this type");
  }
}

// Convenience: the debug rendering as a fresh string.
template <typename T>
std::string DebugString(const T& value) {
  std::string out;
  DebugAppend(out, value);
  return out;
}

}  // namespace opal

#endif  // SMITHY_CORE_PRINT_H_
