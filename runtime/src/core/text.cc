#include "smithy/core/text.h"

namespace opal {

std::size_t Utf8CodePointCount(std::string_view text) {
  std::size_t count = 0;
  for (const char c : text) {
    // Count every byte that is not a UTF-8 continuation byte (10xxxxxx).
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
  }
  return count;
}

}  // namespace opal
