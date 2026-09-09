#ifndef SMITHY_CORE_BLOB_H_
#define SMITHY_CORE_BLOB_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opal {

// Owning byte buffer for Smithy blob shapes. Distinct from std::string to keep
// binary payloads and text from mixing accidentally.
class Blob {
 public:
  Blob() = default;
  explicit Blob(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

  static Blob FromString(std::string_view text) {
    return Blob(std::vector<std::uint8_t>(text.begin(), text.end()));
  }

  std::string ToString() const { return std::string(bytes_.begin(), bytes_.end()); }

  const std::vector<std::uint8_t>& bytes() const { return bytes_; }
  std::vector<std::uint8_t>& bytes() { return bytes_; }
  const std::uint8_t* data() const { return bytes_.data(); }
  std::size_t size() const { return bytes_.size(); }
  bool empty() const { return bytes_.empty(); }

  friend bool operator==(const Blob& a, const Blob& b) { return a.bytes_ == b.bytes_; }
  // Lexicographic by bytes, so blob-bearing generated structs stay orderable.
  friend auto operator<=>(const Blob& a, const Blob& b) { return a.bytes_ <=> b.bytes_; }

  // Debug rendering (smithy/core/print.h): size plus a bounded hex prefix —
  // never full contents, so logging a payload-bearing struct stays sane.
  void AppendDebugTo(std::string& out) const {
    out += "Blob(";
    out += std::to_string(bytes_.size());
    out += " bytes";
    if (!bytes_.empty()) {
      out += ": ";
      static constexpr std::string_view kHex = "0123456789abcdef";
      static constexpr std::size_t kMaxHexBytes = 16;
      const std::size_t shown = bytes_.size() < kMaxHexBytes ? bytes_.size() : kMaxHexBytes;
      for (std::size_t i = 0; i < shown; ++i) {
        out += kHex[bytes_[i] >> 4];
        out += kHex[bytes_[i] & 0xF];
      }
      if (bytes_.size() > kMaxHexBytes) {
        out += "…";
      }
    }
    out += ')';
  }

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace opal

// Hashes by byte content, so blob-bearing generated structs can key unordered
// containers (issue #49). Process-local: never persist hash values.
template <>
struct std::hash<opal::Blob> {
  std::size_t operator()(const opal::Blob& blob) const noexcept {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
  }
};

#endif  // SMITHY_CORE_BLOB_H_
