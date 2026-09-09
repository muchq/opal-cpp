#ifndef OPAL_CORE_BASE64_H_
#define OPAL_CORE_BASE64_H_

#include <string>
#include <string_view>

#include "opal/core/blob.h"
#include "opal/core/outcome.h"

namespace opal {

// Standard base64 (RFC 4648 §4, with padding), as required for blob shapes in
// JSON-based protocols.
std::string Base64Encode(const Blob& blob);

// Rejects non-alphabet characters, bad padding, and non-canonical trailing bits.
Outcome<Blob> Base64Decode(std::string_view text);

}  // namespace opal

#endif  // OPAL_CORE_BASE64_H_
