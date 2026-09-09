#include "opal/core/base64.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace opal {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<std::int8_t, 256> BuildDecodeTable() {
  std::array<std::int8_t, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 64; ++i) {
    table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
  }
  return table;
}

}  // namespace

std::string Base64Encode(const Blob& blob) {
  const auto& bytes = blob.bytes();
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const std::uint32_t group = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                static_cast<std::uint32_t>(bytes[i + 2]);
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.push_back(kAlphabet[(group >> 6) & 0x3F]);
    out.push_back(kAlphabet[group & 0x3F]);
  }
  const std::size_t rest = bytes.size() - i;
  if (rest == 1) {
    const std::uint32_t group = static_cast<std::uint32_t>(bytes[i]) << 16;
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.append("==");
  } else if (rest == 2) {
    const std::uint32_t group = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.push_back(kAlphabet[(group >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

Outcome<Blob> Base64Decode(std::string_view text) {
  static const std::array<std::int8_t, 256> kDecode = BuildDecodeTable();
  if (text.size() % 4 != 0) {
    return Error::Serialization("base64: length must be a multiple of 4");
  }
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    int padding = 0;
    std::uint32_t group = 0;
    for (std::size_t j = 0; j < 4; ++j) {
      const char c = text[i + j];
      if (c == '=') {
        if (!last || j < 2 || (j == 2 && text[i + 3] != '=')) {
          return Error::Serialization("base64: misplaced padding");
        }
        ++padding;
        group <<= 6;
        continue;
      }
      const std::int8_t value = kDecode[static_cast<unsigned char>(c)];
      if (value < 0) return Error::Serialization("base64: invalid character");
      group = (group << 6) | static_cast<std::uint32_t>(value);
    }
    // Canonical encodings leave the unused low bits of the final group zero.
    if ((padding == 1 && (group & 0xFF) != 0) || (padding == 2 && (group & 0xFFFF) != 0)) {
      return Error::Serialization("base64: non-canonical trailing bits");
    }
    out.push_back(static_cast<std::uint8_t>((group >> 16) & 0xFF));
    if (padding < 2) out.push_back(static_cast<std::uint8_t>((group >> 8) & 0xFF));
    if (padding < 1) out.push_back(static_cast<std::uint8_t>(group & 0xFF));
  }
  return Blob(std::move(out));
}

}  // namespace opal
