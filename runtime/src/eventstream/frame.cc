#include "opal/eventstream/frame.h"

#include <algorithm>

#include "opal/core/error.h"
#include "opal/core/overloaded.h"

namespace opal::eventstream {
namespace {

// The format's header wire-type tags. The two boolean tags map onto the
// one bool alternative of HeaderValue; every other tag has its own type.
enum WireType : std::uint8_t {
  kWireBoolTrue = 0,
  kWireBoolFalse = 1,
  kWireByte = 2,
  kWireShort = 3,
  kWireInt = 4,
  kWireLong = 5,
  kWireByteArray = 6,
  kWireString = 7,
  kWireTimestamp = 8,
  kWireUuid = 9,
};

// CRC32, IEEE/zlib polynomial (reflected 0xEDB88320), written out like the
// CBOR codec's primitives (ADR-0014).
std::uint32_t Crc32(std::string_view bytes) {
  static const auto kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
    return table;
  }();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const char byte : bytes) {
    crc = kTable[(crc ^ static_cast<std::uint8_t>(byte)) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

void AppendBigEndian(std::string& out, std::uint64_t value, int bytes) {
  for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

std::uint64_t ReadBigEndian(std::string_view bytes) {
  std::uint64_t value = 0;
  for (const char byte : bytes) {
    value = (value << 8) | static_cast<std::uint8_t>(byte);
  }
  return value;
}

// Encodes one header's type tag + value; returns false only for the
// bounded-length violations the caller reports (string/byte-array values
// carry a u16 length).
bool EncodeValue(std::string& out, const HeaderValue& value) {
  return std::visit(Overloaded{
                        [&](bool b) {
                          out.push_back(static_cast<char>(b ? kWireBoolTrue : kWireBoolFalse));
                          return true;
                        },
                        [&](std::int8_t v) {
                          out.push_back(static_cast<char>(kWireByte));
                          out.push_back(static_cast<char>(v));
                          return true;
                        },
                        [&](std::int16_t v) {
                          out.push_back(static_cast<char>(kWireShort));
                          AppendBigEndian(out, static_cast<std::uint16_t>(v), 2);
                          return true;
                        },
                        [&](std::int32_t v) {
                          out.push_back(static_cast<char>(kWireInt));
                          AppendBigEndian(out, static_cast<std::uint32_t>(v), 4);
                          return true;
                        },
                        [&](std::int64_t v) {
                          out.push_back(static_cast<char>(kWireLong));
                          AppendBigEndian(out, static_cast<std::uint64_t>(v), 8);
                          return true;
                        },
                        [&](const Blob& v) {
                          if (v.size() > 0xFFFF) return false;
                          out.push_back(static_cast<char>(kWireByteArray));
                          AppendBigEndian(out, v.size(), 2);
                          out.append(reinterpret_cast<const char*>(v.data()), v.size());
                          return true;
                        },
                        [&](const std::string& v) {
                          if (v.size() > 0xFFFF) return false;
                          out.push_back(static_cast<char>(kWireString));
                          AppendBigEndian(out, v.size(), 2);
                          out += v;
                          return true;
                        },
                        [&](const Timestamp& v) {
                          out.push_back(static_cast<char>(kWireTimestamp));
                          AppendBigEndian(out, static_cast<std::uint64_t>(v.epoch_milliseconds()),
                                          8);
                          return true;
                        },
                        [&](const Uuid& v) {
                          out.push_back(static_cast<char>(kWireUuid));
                          out.append(reinterpret_cast<const char*>(v.bytes.data()), v.bytes.size());
                          return true;
                        },
                    },
                    value);
}

// Decodes one header value starting at `cursor` within the header block;
// nullopt for an unknown wire type or a value that runs past the block.
// Advances `cursor` past the value on success.
std::optional<HeaderValue> DecodeValue(std::string_view block, std::size_t& cursor) {
  if (cursor >= block.size()) {
    return std::nullopt;
  }
  const auto type = static_cast<std::uint8_t>(block[cursor++]);
  const auto fixed = [&](std::size_t n) -> std::optional<std::string_view> {
    if (block.size() - cursor < n) return std::nullopt;
    const std::string_view bytes = block.substr(cursor, n);
    cursor += n;
    return bytes;
  };
  switch (type) {
    case kWireBoolTrue:
      return HeaderValue{true};
    case kWireBoolFalse:
      return HeaderValue{false};
    case kWireByte: {
      const auto bytes = fixed(1);
      if (!bytes) return std::nullopt;
      return HeaderValue{static_cast<std::int8_t>(ReadBigEndian(*bytes))};
    }
    case kWireShort: {
      const auto bytes = fixed(2);
      if (!bytes) return std::nullopt;
      return HeaderValue{static_cast<std::int16_t>(ReadBigEndian(*bytes))};
    }
    case kWireInt: {
      const auto bytes = fixed(4);
      if (!bytes) return std::nullopt;
      return HeaderValue{static_cast<std::int32_t>(ReadBigEndian(*bytes))};
    }
    case kWireLong: {
      const auto bytes = fixed(8);
      if (!bytes) return std::nullopt;
      return HeaderValue{static_cast<std::int64_t>(ReadBigEndian(*bytes))};
    }
    case kWireByteArray: {
      const auto length_bytes = fixed(2);
      if (!length_bytes) return std::nullopt;
      const auto bytes = fixed(ReadBigEndian(*length_bytes));
      if (!bytes) return std::nullopt;
      return HeaderValue{Blob::FromString(*bytes)};
    }
    case kWireString: {
      const auto length_bytes = fixed(2);
      if (!length_bytes) return std::nullopt;
      const auto bytes = fixed(ReadBigEndian(*length_bytes));
      if (!bytes) return std::nullopt;
      return HeaderValue{std::string(*bytes)};
    }
    case kWireTimestamp: {
      const auto bytes = fixed(8);
      if (!bytes) return std::nullopt;
      // The unchecked factory: int64 wire extremes round-trip verbatim
      // (only string formatting is range-limited, and headers never format).
      return HeaderValue{
          Timestamp::FromEpochMilliseconds(static_cast<std::int64_t>(ReadBigEndian(*bytes)))};
    }
    case kWireUuid: {
      const auto bytes = fixed(16);
      if (!bytes) return std::nullopt;
      Uuid uuid;
      std::copy(bytes->begin(), bytes->end(), uuid.bytes.begin());
      return HeaderValue{uuid};
    }
    default:
      return std::nullopt;  // unknown wire type: a hard error, not a skip
  }
}

constexpr std::size_t kPreludeBytes = 12;  // total + headers length + prelude CRC
constexpr std::size_t kFrameOverheadBytes = kPreludeBytes + 4;  // + message CRC

// The decode-side rejection, prefixed once (the cbor codec's Fail shape).
Error Malformed(const char* what) {
  return Error::Serialization(std::string("eventstream: ") + what);
}

}  // namespace

const HeaderValue* Message::FindHeader(std::string_view name) const {
  for (const Header& header : headers) {
    if (header.name == name) {
      return &header.value;
    }
  }
  return nullptr;
}

const std::string* Message::FindString(std::string_view name) const {
  const HeaderValue* value = FindHeader(name);
  return value != nullptr ? std::get_if<std::string>(value) : nullptr;
}

Outcome<std::string> EncodeMessage(const Message& message) {
  std::string headers;
  for (const Header& header : message.headers) {
    if (header.name.empty() || header.name.size() > 255) {
      return Error::Validation("eventstream: header name must be 1..255 bytes");
    }
    headers.push_back(static_cast<char>(header.name.size()));
    headers += header.name;
    if (!EncodeValue(headers, header.value)) {
      return Error::Validation("eventstream: header value over 64 KiB: " + header.name);
    }
  }
  if (headers.size() > kMaxHeaderBlockBytes) {
    return Error::Validation("eventstream: header block over the 128 KiB limit");
  }
  const std::size_t total = kFrameOverheadBytes + headers.size() + message.payload.size();
  if (total > kMaxMessageBytes) {
    return Error::Validation("eventstream: message over the 16 MiB limit");
  }

  std::string frame;
  frame.reserve(total);
  AppendBigEndian(frame, total, 4);
  AppendBigEndian(frame, headers.size(), 4);
  AppendBigEndian(frame, Crc32(frame), 4);  // prelude CRC: the first 8 bytes
  frame += headers;
  frame.append(reinterpret_cast<const char*>(message.payload.data()), message.payload.size());
  AppendBigEndian(frame, Crc32(frame), 4);  // message CRC: everything before it
  return frame;
}

Outcome<std::optional<DecodedFrame>> DecodeMessage(std::string_view buffer) {
  if (buffer.size() < kPreludeBytes) {
    return std::optional<DecodedFrame>();  // feed me more
  }
  // The prelude CRC is verified before either declared length is trusted:
  // a corrupted length must not drive buffering or allocation decisions.
  const auto prelude_crc = static_cast<std::uint32_t>(ReadBigEndian(buffer.substr(8, 4)));
  if (Crc32(buffer.substr(0, 8)) != prelude_crc) {
    return Malformed("prelude CRC mismatch");
  }
  const auto total = static_cast<std::size_t>(ReadBigEndian(buffer.substr(0, 4)));
  const auto headers_length = static_cast<std::size_t>(ReadBigEndian(buffer.substr(4, 4)));
  if (total > kMaxMessageBytes || total < kFrameOverheadBytes ||
      headers_length > kMaxHeaderBlockBytes || headers_length > total - kFrameOverheadBytes) {
    return Malformed("declared lengths out of bounds");
  }
  if (buffer.size() < total) {
    return std::optional<DecodedFrame>();  // feed me more
  }
  const auto message_crc = static_cast<std::uint32_t>(ReadBigEndian(buffer.substr(total - 4, 4)));
  if (Crc32(buffer.substr(0, total - 4)) != message_crc) {
    return Malformed("message CRC mismatch");
  }

  Message message;
  const std::string_view block = buffer.substr(kPreludeBytes, headers_length);
  std::size_t cursor = 0;
  while (cursor < block.size()) {
    const auto name_length = static_cast<std::uint8_t>(block[cursor++]);
    if (name_length == 0 || block.size() - cursor < name_length) {
      return Malformed("malformed header name");
    }
    std::string name(block.substr(cursor, name_length));
    cursor += name_length;
    auto value = DecodeValue(block, cursor);
    if (!value.has_value()) {
      return Malformed("malformed header value");
    }
    message.headers.push_back(Header{std::move(name), std::move(*value)});
  }
  // The bounds checks above keep cursor <= block.size() at every step, so
  // the loop exits exactly at the block boundary.

  message.payload = Blob::FromString(
      buffer.substr(kPreludeBytes + headers_length, total - kFrameOverheadBytes - headers_length));
  return std::optional<DecodedFrame>(DecodedFrame{std::move(message), total});
}

}  // namespace opal::eventstream
