#include "smithy/core/uuid.h"

#include <array>
#include <cstdint>
#include <random>
#include <string_view>

namespace opal {

std::string GenerateUuidV4() {
  thread_local std::mt19937_64 generator = [] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device()};
    return std::mt19937_64(seed);
  }();
  std::uniform_int_distribution<std::uint64_t> distribution;
  std::uint64_t high = distribution(generator);
  std::uint64_t low = distribution(generator);
  high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
  low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;    // variant 10xx

  static constexpr std::string_view kHex = "0123456789abcdef";
  static constexpr std::array<int, 16> kDashAfter = {0, 0, 0, 1, 0, 1, 0, 1,
                                                     0, 1, 0, 0, 0, 0, 0, 0};
  std::string out;
  out.reserve(36);
  const std::array<std::uint64_t, 2> words = {high, low};
  int byte_index = 0;
  for (const std::uint64_t word : words) {
    for (int shift = 56; shift >= 0; shift -= 8, ++byte_index) {
      const auto byte = static_cast<unsigned>((word >> shift) & 0xFF);
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0xF]);
      if (kDashAfter[byte_index] != 0) {
        out.push_back('-');
      }
    }
  }
  return out;
}

}  // namespace opal
