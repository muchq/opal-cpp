#include "smithy/cbor/cbor.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace opal::cbor {
namespace {

// --- Encoding ---------------------------------------------------------------

enum Major : std::uint8_t {
  kUnsigned = 0,
  kNegative = 1,
  kBytes = 2,
  kText = 3,
  kArray = 4,
  kMap = 5,
  kTag = 6,
  kSimple = 7,
};

constexpr std::uint8_t kBreak = 0xFF;
constexpr std::uint8_t kSimpleFalse = 20;
constexpr std::uint8_t kSimpleTrue = 21;
constexpr std::uint8_t kSimpleNull = 22;
constexpr std::uint8_t kSimpleUndefined = 23;
constexpr std::uint64_t kTagEpochTimestamp = 1;

void WriteTypeAndValue(std::vector<std::uint8_t>* out, Major major, std::uint64_t value) {
  const auto initial = static_cast<std::uint8_t>(major << 5);
  if (value < 24) {
    out->push_back(initial | static_cast<std::uint8_t>(value));
  } else if (value <= 0xFF) {
    out->push_back(initial | 24);
    out->push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xFFFF) {
    out->push_back(initial | 25);
    for (int shift = 8; shift >= 0; shift -= 8) {
      out->push_back(static_cast<std::uint8_t>(value >> shift));
    }
  } else if (value <= 0xFFFFFFFF) {
    out->push_back(initial | 26);
    for (int shift = 24; shift >= 0; shift -= 8) {
      out->push_back(static_cast<std::uint8_t>(value >> shift));
    }
  } else {
    out->push_back(initial | 27);
    for (int shift = 56; shift >= 0; shift -= 8) {
      out->push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }
}

void WriteInt(std::vector<std::uint8_t>* out, std::int64_t value) {
  if (value >= 0) {
    WriteTypeAndValue(out, kUnsigned, static_cast<std::uint64_t>(value));
  } else {
    WriteTypeAndValue(out, kNegative, static_cast<std::uint64_t>(-(value + 1)));
  }
}

void WriteDouble(std::vector<std::uint8_t>* out, double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  out->push_back((kSimple << 5) | 27);
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<std::uint8_t>(bits >> shift));
  }
}

void EncodeValue(std::vector<std::uint8_t>* out, const Document& doc) {
  if (doc.is_null()) {
    out->push_back((kSimple << 5) | kSimpleNull);
  } else if (doc.is_bool()) {
    out->push_back((kSimple << 5) | (doc.as_bool() ? kSimpleTrue : kSimpleFalse));
  } else if (doc.is_int()) {
    WriteInt(out, doc.as_int());
  } else if (doc.is_double()) {
    WriteDouble(out, doc.as_double());
  } else if (doc.is_string()) {
    const std::string& text = doc.as_string();
    WriteTypeAndValue(out, kText, text.size());
    out->insert(out->end(), text.begin(), text.end());
  } else if (doc.is_blob()) {
    const auto& bytes = doc.as_blob().bytes();
    WriteTypeAndValue(out, kBytes, bytes.size());
    out->insert(out->end(), bytes.begin(), bytes.end());
  } else if (doc.is_timestamp()) {
    WriteTypeAndValue(out, kTag, kTagEpochTimestamp);
    const std::int64_t ms = doc.as_timestamp().value.epoch_milliseconds();
    if (ms % 1000 == 0) {
      WriteInt(out, ms / 1000);
    } else {
      WriteDouble(out, static_cast<double>(ms) / 1000.0);
    }
  } else if (doc.is_list()) {
    const DocumentList& list = doc.as_list();
    WriteTypeAndValue(out, kArray, list.size());
    for (const Document& item : list) EncodeValue(out, item);
  } else {
    const DocumentMap& map = doc.as_map();
    WriteTypeAndValue(out, kMap, map.size());
    for (const auto& [key, value] : map) {
      WriteTypeAndValue(out, kText, key.size());
      out->insert(out->end(), key.begin(), key.end());
      EncodeValue(out, value);
    }
  }
}

// --- Decoding ---------------------------------------------------------------

class Decoder {
 public:
  Decoder(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  Outcome<Document> DecodeDocument() {
    auto doc = DecodeValue(kMaxDepth);
    if (!doc) return doc;
    if (pos_ != size_) return Fail("trailing bytes after value");
    return doc;
  }

 private:
  static constexpr int kMaxDepth = 64;

  static Error Fail(const std::string& what) { return Error::Serialization("cbor: " + what); }

  bool Take(std::uint8_t* byte) {
    if (pos_ >= size_) return false;
    *byte = data_[pos_++];
    return true;
  }

  bool TakeBigEndian(int bytes, std::uint64_t* value) {
    if (pos_ + static_cast<std::size_t>(bytes) > size_) return false;
    *value = 0;
    for (int i = 0; i < bytes; ++i) *value = (*value << 8) | data_[pos_++];
    return true;
  }

  static double DecodeHalf(std::uint64_t bits) {
    const unsigned sign = (bits >> 15) & 0x1;
    const unsigned exponent = (bits >> 10) & 0x1F;
    const unsigned mantissa = bits & 0x3FF;
    double value = 0;
    if (exponent == 0) {
      value = std::ldexp(mantissa, -24);
    } else if (exponent == 31) {
      value = mantissa == 0 ? std::numeric_limits<double>::infinity()
                            : std::numeric_limits<double>::quiet_NaN();
    } else {
      value = std::ldexp(mantissa + 1024, static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -value : value;
  }

  // Reads the argument for an initial byte; `indefinite` reports 0x1F lengths.
  Outcome<std::uint64_t> ReadArgument(std::uint8_t info, bool* indefinite) {
    *indefinite = false;
    if (info < 24) return std::uint64_t{info};
    if (info == 24 || info == 25 || info == 26 || info == 27) {
      const int bytes = 1 << (info - 24);
      std::uint64_t value = 0;
      if (!TakeBigEndian(bytes, &value)) return Fail("truncated argument");
      return value;
    }
    if (info == 31) {
      *indefinite = true;
      return std::uint64_t{0};
    }
    return Fail("reserved additional-information value");
  }

  Outcome<std::string> DecodeChunkedString(Major major, bool indefinite, std::uint64_t length) {
    std::string out;
    if (!indefinite) {
      // length is attacker-controlled and may be near 2^64: compare against
      // the remaining bytes so the check cannot overflow and wrap.
      if (length > size_ - pos_) return Fail("truncated string");
      out.assign(reinterpret_cast<const char*>(data_ + pos_), length);
      pos_ += length;
      return out;
    }
    // Indefinite strings are chunks of the same major type until a break.
    while (true) {
      std::uint8_t initial = 0;
      if (!Take(&initial)) return Fail("unterminated indefinite string");
      if (initial == kBreak) return out;
      if ((initial >> 5) != major) return Fail("mixed types in indefinite string");
      bool chunk_indefinite = false;
      auto chunk_length = ReadArgument(initial & 0x1F, &chunk_indefinite);
      if (!chunk_length) return std::move(chunk_length).error();
      if (chunk_indefinite) return Fail("nested indefinite string chunk");
      if (*chunk_length > size_ - pos_) return Fail("truncated string chunk");
      out.append(reinterpret_cast<const char*>(data_ + pos_), *chunk_length);
      pos_ += *chunk_length;
    }
  }

  Outcome<Document> DecodeValue(int depth) {  // NOLINT(misc-no-recursion)
    if (depth <= 0) return Fail("nesting too deep");
    std::uint8_t initial = 0;
    if (!Take(&initial)) return Fail("truncated input");
    const auto major = static_cast<Major>(initial >> 5);
    const std::uint8_t info = initial & 0x1F;
    bool indefinite = false;

    switch (major) {
      case kUnsigned: {
        auto value = ReadArgument(info, &indefinite);
        if (!value) return std::move(value).error();
        // RFC 8949 §3.3: additional information 31 is well-formed only for
        // strings, arrays, and maps; an "indefinite integer" is not CBOR.
        if (indefinite) return Fail("indefinite length on integer");
        if (*value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return Fail("integer exceeds int64 range");
        }
        return Document(static_cast<std::int64_t>(*value));
      }
      case kNegative: {
        auto value = ReadArgument(info, &indefinite);
        if (!value) return std::move(value).error();
        if (indefinite) return Fail("indefinite length on integer");
        if (*value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return Fail("integer exceeds int64 range");
        }
        return Document(-1 - static_cast<std::int64_t>(*value));
      }
      case kBytes: {
        auto length = ReadArgument(info, &indefinite);
        if (!length) return std::move(length).error();
        auto text = DecodeChunkedString(kBytes, indefinite, *length);
        if (!text) return std::move(text).error();
        return Document(Blob::FromString(*text));
      }
      case kText: {
        auto length = ReadArgument(info, &indefinite);
        if (!length) return std::move(length).error();
        auto text = DecodeChunkedString(kText, indefinite, *length);
        if (!text) return std::move(text).error();
        return Document(std::move(*text));
      }
      case kArray: {
        auto length = ReadArgument(info, &indefinite);
        if (!length) return std::move(length).error();
        DocumentList list;
        for (std::uint64_t i = 0; indefinite || i < *length; ++i) {
          if (indefinite && pos_ < size_ && data_[pos_] == kBreak) {
            ++pos_;
            break;
          }
          auto item = DecodeValue(depth - 1);
          if (!item) return std::move(item).error();
          list.push_back(std::move(*item));
        }
        return Document(std::move(list));
      }
      case kMap: {
        auto length = ReadArgument(info, &indefinite);
        if (!length) return std::move(length).error();
        DocumentMap map;
        for (std::uint64_t i = 0; indefinite || i < *length; ++i) {
          if (indefinite && pos_ < size_ && data_[pos_] == kBreak) {
            ++pos_;
            break;
          }
          auto key = DecodeValue(depth - 1);
          if (!key) return std::move(key).error();
          if (!key->is_string()) return Fail("map key is not a text string");
          auto value = DecodeValue(depth - 1);
          if (!value) return std::move(value).error();
          map.insert_or_assign(key->as_string(), std::move(*value));
        }
        return Document(std::move(map));
      }
      case kTag: {
        auto tag = ReadArgument(info, &indefinite);
        if (!tag) return std::move(tag).error();
        if (indefinite) return Fail("indefinite length on tag");
        auto inner = DecodeValue(depth - 1);
        if (!inner) return std::move(inner).error();
        if (*tag == kTagEpochTimestamp) {
          // Tag 1 content is epoch seconds (integer or float). Route both
          // through the range-checked factory: a large integer would overflow
          // int64 when scaled to milliseconds, and an out-of-range float would
          // overflow the cast — both undefined behavior on untrusted input.
          if (inner->is_int()) {
            auto ts = Timestamp::FromEpochSecondsChecked(static_cast<double>(inner->as_int()));
            if (!ts) return std::move(ts).error();
            return Document(TimestampValue{*ts, TimestampFormat::kEpochSeconds});
          }
          if (inner->is_double()) {
            auto ts = Timestamp::FromEpochSecondsChecked(inner->as_double());
            if (!ts) return std::move(ts).error();
            return Document(TimestampValue{*ts, TimestampFormat::kEpochSeconds});
          }
          return Fail("tag 1 content is not a number");
        }
        return inner;  // Unknown tags are ignored; the inner value stands alone.
      }
      case kSimple: {
        if (info == kSimpleFalse) return Document(false);
        if (info == kSimpleTrue) return Document(true);
        if (info == kSimpleNull || info == kSimpleUndefined) return Document(nullptr);
        if (info == 25 || info == 26 || info == 27) {
          std::uint64_t bits = 0;
          if (!TakeBigEndian(1 << (info - 24), &bits)) return Fail("truncated float");
          if (info == 25) return Document(DecodeHalf(bits));
          if (info == 26) {
            float value = 0;
            const auto narrow = static_cast<std::uint32_t>(bits);
            std::memcpy(&value, &narrow, sizeof(value));
            return Document(static_cast<double>(value));
          }
          double value = 0;
          std::memcpy(&value, &bits, sizeof(value));
          return Document(value);
        }
        return Fail("unsupported simple value");
      }
    }
    return Fail("unreachable");
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

}  // namespace

Blob Encode(const Document& doc) {
  std::vector<std::uint8_t> out;
  EncodeValue(&out, doc);
  return Blob(std::move(out));
}

Outcome<Document> Decode(const Blob& bytes) {
  return Decoder(bytes.data(), bytes.size()).DecodeDocument();
}

}  // namespace opal::cbor
