#include "smithy/core/version.h"

namespace opal {

// The one source of truth for the product version (see docs/versioning.md);
// the client User-Agent mirrors it.
std::string_view Version() { return "0.3.0-dev"; }

}  // namespace opal
