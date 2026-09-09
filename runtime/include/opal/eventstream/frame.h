#ifndef OPAL_EVENTSTREAM_FRAME_H_
#define OPAL_EVENTSTREAM_FRAME_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "opal/core/blob.h"
#include "opal/core/outcome.h"
#include "opal/core/print.h"
#include "opal/core/timestamp.h"

namespace opal::eventstream {

// The event-stream message framing the protocol specs define event streams
// against (ADR-0014): a CRC-guarded prelude, a typed header block, and an
// opaque payload. This layer carries protocol bytes; it does not interpret
// them.
//
//   [ total length (u32 BE) | headers length (u32 BE) | prelude CRC32 ]
//   [ headers block ][ payload ][ message CRC32 ]
//
// Timestamps and UUIDs are distinct value types rather than aliases of
// long/byte-array, so a decoded header can never be confused across wire
// types that share a C++ representation. The timestamp is the runtime's
// one `opal::Timestamp`, so headers and generated code trade timestamps
// without a conversion; core has no UUID value type, so Uuid lives here.
struct Uuid {
  std::array<std::uint8_t, 16> bytes{};
  friend bool operator==(const Uuid&, const Uuid&) = default;

  // Debug rendering (opal/core/print.h): canonical dashed hex.
  void AppendDebugTo(std::string& out) const {
    static constexpr std::string_view kHex = "0123456789abcdef";
    out += "Uuid(";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) {
        out += '-';
      }
      out += kHex[bytes[i] >> 4];
      out += kHex[bytes[i] & 0xF];
    }
    out += ')';
  }
};

// One alternative per wire type; the two boolean wire tags collapse into
// the one bool alternative (the tag is recovered from the value on encode).
// Construction is by plain values — `Header{"name", "text"}` selects the
// string alternative (a string literal can never land on bool under C++20
// variant rules) and a plain `int` selects int32; only byte and short need
// their explicit `std::int8_t` / `std::int16_t` spellings.
using HeaderValue = std::variant<bool, std::int8_t, std::int16_t, std::int32_t, std::int64_t, Blob,
                                 std::string, Timestamp, Uuid>;

struct Header {
  std::string name;
  HeaderValue value;
  friend bool operator==(const Header&, const Header&) = default;

  void AppendDebugTo(std::string& out) const {
    DebugAppend(out, name);
    out += ": ";
    std::visit([&out](const auto& alternative) { DebugAppend(out, alternative); }, value);
  }
};

// Both members carry default member initializers so partial aggregate
// initialization (`Message{{...headers...}}`) is warning-free under -Wextra's
// -Wmissing-field-initializers — here and in consumer code.
struct Message {
  std::vector<Header> headers{};
  Blob payload{};
  friend bool operator==(const Message&, const Message&) = default;

  // First header with this name, or nullptr. Linear: header counts are
  // small. Duplicate names are preserved by the codec; their meaning is
  // the layer above's call.
  const HeaderValue* FindHeader(std::string_view name) const;
  // The string value of the first header with this name; nullptr when
  // absent or holding a different wire type — the shape of the
  // `:event-type`-style dispatch every consumer does. Other types:
  // FindHeader + std::get_if.
  const std::string* FindString(std::string_view name) const;

  void AppendDebugTo(std::string& out) const {
    out += "Message(";
    DebugAppend(out, headers);
    out += ", payload=";
    payload.AppendDebugTo(out);
    out += ')';
  }
};

// The format's own documented limits, enforced symmetrically: Encode
// refuses what Decode would reject, so a message that cannot round-trip
// cannot be produced. Header names are 1..255 bytes (u8 length prefix);
// string and byte-array header values are bounded by their u16 length.
inline constexpr std::size_t kMaxHeaderBlockBytes = std::size_t{128} * 1024;
inline constexpr std::size_t kMaxMessageBytes = std::size_t{16} * 1024 * 1024;

// Encodes one message as a complete frame. Validation errors (an empty or
// oversized header name, an oversized string/byte-array value, a header
// block or total frame over the limits) fail the encode — nothing partial
// is produced. Frames travel as std::string (unlike the cbor codec's Blob)
// because they splice into transport buffers, which are string-shaped;
// the payload INSIDE stays a Blob.
Outcome<std::string> EncodeMessage(const Message& message);

// One message decoded from the FRONT of `buffer`, and how many bytes its
// frame consumed (the caller drops them and calls again — the decoder is
// incremental because transports feed it from sockets).
struct DecodedFrame {
  Message message;
  std::size_t bytes_consumed = 0;
};

// nullopt means the buffer does not yet hold a complete frame: feed more
// bytes, it is never an error. Errors are permanent for the stream —
// a prelude or message CRC mismatch, a declared length outside the
// format's bounds, a header block that overruns or underruns its declared
// length, or an unknown header wire type. The prelude CRC is verified
// before any declared length is trusted; the message CRC before any
// content is surfaced.
//
// Canonical transport loop (`buffer` accumulates unconsumed socket bytes):
//
//   auto decoded = DecodeMessage(buffer);
//   if (!decoded.ok()) return decoded.error();   // stream is dead: stop feeding
//   if (!decoded->has_value()) break;            // read more bytes, come back
//   Handle((**decoded).message);
//   buffer.erase(0, (**decoded).bytes_consumed); // or advance an offset
//
// Check ok() before has_value(): dereferencing the value side of an error
// Outcome is fatal (ADR-0009), and **decoded on an incomplete buffer is an
// unchecked optional dereference.
Outcome<std::optional<DecodedFrame>> DecodeMessage(std::string_view buffer);

}  // namespace opal::eventstream

#endif  // OPAL_EVENTSTREAM_FRAME_H_
