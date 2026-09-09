#include "smithy/json/json.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

#include "smithy/core/base64.h"

namespace opal::json {
namespace {

nlohmann::json ToBackend(const Document& doc) {
  if (doc.is_null()) return nullptr;
  if (doc.is_bool()) return doc.as_bool();
  if (doc.is_int()) return doc.as_int();
  if (doc.is_double()) {
    const double value = doc.as_double();
    // Smithy JSON protocols encode non-finite numbers as strings.
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    return value;
  }
  if (doc.is_string()) return doc.as_string();
  if (doc.is_blob()) return Base64Encode(doc.as_blob());
  if (doc.is_timestamp()) {
    const TimestampValue& ts = doc.as_timestamp();
    if (ts.format == TimestampFormat::kEpochSeconds) {
      const double seconds = ts.value.epoch_seconds();
      const auto whole = static_cast<std::int64_t>(seconds);
      if (static_cast<double>(whole) == seconds) return whole;
      return seconds;
    }
    return ts.value.Format(ts.format);
  }
  if (doc.is_list()) {
    nlohmann::json out = nlohmann::json::array();
    for (const Document& item : doc.as_list()) out.push_back(ToBackend(item));
    return out;
  }
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [key, value] : doc.as_map()) out[key] = ToBackend(value);
  return out;
}

// Deeply nested JSON would overflow the stack in nlohmann's recursive-descent
// parser (and again in FromBackend) before any structural limit applies. This
// O(n), iterative pre-scan rejects input past a fixed nesting depth so a
// hostile body can't crash the process — the counterpart to CBOR's DecodeValue
// depth guard. Brackets inside strings don't nest, so track string state.
constexpr int kMaxNestingDepth = 512;

bool ExceedsMaxDepth(std::string_view text) {
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char c : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    switch (c) {
      case '"':
        in_string = true;
        break;
      case '[':
      case '{':
        if (++depth > kMaxNestingDepth) return true;
        break;
      case ']':
      case '}':
        --depth;
        break;
      default:
        break;
    }
  }
  return false;
}

Outcome<Document> FromBackend(const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
      return Document(nullptr);
    case nlohmann::json::value_t::boolean:
      return Document(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
      return Document(value.get<std::int64_t>());
    case nlohmann::json::value_t::number_unsigned: {
      const auto raw = value.get<std::uint64_t>();
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Error::Serialization("json: integer exceeds int64 range");
      }
      return Document(static_cast<std::int64_t>(raw));
    }
    case nlohmann::json::value_t::number_float:
      return Document(value.get<double>());
    case nlohmann::json::value_t::string:
      return Document(value.get<std::string>());
    case nlohmann::json::value_t::array: {
      DocumentList list;
      list.reserve(value.size());
      for (const auto& item : value) {
        auto converted = FromBackend(item);
        if (!converted) return std::move(converted).error();
        list.push_back(std::move(*converted));
      }
      return Document(std::move(list));
    }
    case nlohmann::json::value_t::object: {
      DocumentMap map;
      for (const auto& [key, item] : value.items()) {
        auto converted = FromBackend(item);
        if (!converted) return std::move(converted).error();
        map.insert_or_assign(key, std::move(*converted));
      }
      return Document(std::move(map));
    }
    default:
      return Error::Serialization("json: unsupported value type");
  }
}

}  // namespace

std::string Encode(const Document& doc) {
  // error_handler_t::replace: substitute U+FFFD for invalid UTF-8 instead of
  // throwing (nlohmann's default). Strings can carry raw bytes from @httpLabel
  // segments, headers, or blobs echoed into a response; a strict dump would
  // throw type_error.316 uncaught and terminate the server. Replacement keeps
  // the output valid JSON and the process alive.
  return ToBackend(doc).dump(/*indent=*/-1, /*indent_char=*/' ',
                             /*ensure_ascii=*/false, nlohmann::json::error_handler_t::replace);
}

Outcome<Document> Decode(std::string_view text) {
  if (ExceedsMaxDepth(text)) {
    return Error::Serialization("json: nesting too deep");
  }
  const nlohmann::json parsed = nlohmann::json::parse(text, /*cb=*/nullptr,
                                                      /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return Error::Serialization("json: malformed document");
  }
  return FromBackend(parsed);
}

}  // namespace opal::json
