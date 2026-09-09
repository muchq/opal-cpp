#ifndef SMITHY_CORE_DOCUMENT_H_
#define SMITHY_CORE_DOCUMENT_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "smithy/core/blob.h"
#include "smithy/core/print.h"
#include "smithy/core/timestamp.h"

namespace opal {

class Document;
using DocumentList = std::vector<Document>;
using DocumentMap = std::map<std::string, Document, std::less<>>;

// A timestamp captured together with the wire format it should serialize to;
// the format comes from the protocol default or an @timestampFormat trait.
struct TimestampValue {
  Timestamp value;
  TimestampFormat format = TimestampFormat::kEpochSeconds;

  friend bool operator==(const TimestampValue& a, const TimestampValue& b) {
    return a.value == b.value && a.format == b.format;
  }

  // Debug rendering: the instant; the wire format is serde detail.
  void AppendDebugTo(std::string& out) const { value.AppendDebugTo(out); }
};

// Dynamic structured value: the pivot representation between typed shapes and
// the wire codecs (smithy/json, smithy/cbor). Covers the Smithy document type
// (null/bool/number/string/list/map) plus blob and timestamp nodes so serde
// code can hand binary and time values to the codec that knows how to render
// them (base64 vs byte string, RFC 3339 vs CBOR tag 1, ...).
class Document {
 public:
  using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Blob,
                             TimestampValue, DocumentList, DocumentMap>;

  Document() : value_(nullptr) {}
  Document(std::nullptr_t) : value_(nullptr) {}                // NOLINT
  Document(bool value) : value_(value) {}                      // NOLINT
  Document(int value) : value_(std::int64_t{value}) {}         // NOLINT
  Document(std::int64_t value) : value_(value) {}              // NOLINT
  Document(double value) : value_(value) {}                    // NOLINT
  Document(const char* value) : value_(std::string(value)) {}  // NOLINT
  Document(std::string value) : value_(std::move(value)) {}    // NOLINT
  Document(Blob value) : value_(std::move(value)) {}           // NOLINT
  Document(TimestampValue value) : value_(value) {}            // NOLINT
  Document(DocumentList value) : value_(std::move(value)) {}   // NOLINT
  Document(DocumentMap value) : value_(std::move(value)) {}    // NOLINT

  static Document FromTimestamp(Timestamp ts, TimestampFormat format) {
    return Document(TimestampValue{ts, format});
  }

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
  bool is_bool() const { return std::holds_alternative<bool>(value_); }
  bool is_int() const { return std::holds_alternative<std::int64_t>(value_); }
  bool is_double() const { return std::holds_alternative<double>(value_); }
  bool is_string() const { return std::holds_alternative<std::string>(value_); }
  bool is_blob() const { return std::holds_alternative<Blob>(value_); }
  bool is_timestamp() const { return std::holds_alternative<TimestampValue>(value_); }
  bool is_list() const { return std::holds_alternative<DocumentList>(value_); }
  bool is_map() const { return std::holds_alternative<DocumentMap>(value_); }

  // Preconditions: the corresponding is_*() holds.
  bool as_bool() const { return std::get<bool>(value_); }
  std::int64_t as_int() const { return std::get<std::int64_t>(value_); }
  double as_double() const { return std::get<double>(value_); }
  const std::string& as_string() const { return std::get<std::string>(value_); }
  const Blob& as_blob() const { return std::get<Blob>(value_); }
  const TimestampValue& as_timestamp() const { return std::get<TimestampValue>(value_); }
  const DocumentList& as_list() const { return std::get<DocumentList>(value_); }
  const DocumentMap& as_map() const { return std::get<DocumentMap>(value_); }
  DocumentList& as_list() { return std::get<DocumentList>(value_); }
  DocumentMap& as_map() { return std::get<DocumentMap>(value_); }

  // Numeric convenience: an integer node read as double, or vice versa.
  double AsNumber() const { return is_int() ? static_cast<double>(as_int()) : as_double(); }

  // Map convenience: nullptr when absent (or when this is not a map).
  const Document* Find(std::string_view key) const {
    if (!is_map()) return nullptr;
    const auto& map = as_map();
    const auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
  }

  const Value& value() const { return value_; }

  // Debug rendering (smithy/core/print.h): JSON-ish — null/true/7/"x"/[..]/
  // {"k": v}, with blob and timestamp nodes reusing their own renderings.
  // For humans only; the JSON codec is the parseable form.
  void AppendDebugTo(std::string& out) const {
    std::visit(
        [&out](const auto& node) {
          if constexpr (std::is_same_v<std::decay_t<decltype(node)>, std::nullptr_t>) {
            out += "null";
          } else {
            DebugAppend(out, node);
          }
        },
        value_);
  }

  friend bool operator==(const Document& a, const Document& b) { return a.value_ == b.value_; }

 private:
  Value value_;
};

}  // namespace opal

#endif  // SMITHY_CORE_DOCUMENT_H_
