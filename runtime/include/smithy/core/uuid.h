#ifndef SMITHY_CORE_UUID_H_
#define SMITHY_CORE_UUID_H_

#include <string>

namespace opal {

// Random (version 4) UUID in canonical 8-4-4-4-12 hex form. Used by generated
// clients to fill unset @idempotencyToken members. Not for cryptographic use.
std::string GenerateUuidV4();

}  // namespace opal

#endif  // SMITHY_CORE_UUID_H_
