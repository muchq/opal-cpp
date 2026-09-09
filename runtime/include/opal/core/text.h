#ifndef OPAL_CORE_TEXT_H_
#define OPAL_CORE_TEXT_H_

#include <cstddef>
#include <string_view>

namespace opal {

// Number of Unicode code points in UTF-8 text (Smithy @length counts code
// points for strings, not bytes). Invalid bytes count as one code point each.
std::size_t Utf8CodePointCount(std::string_view text);

}  // namespace opal

#endif  // OPAL_CORE_TEXT_H_
