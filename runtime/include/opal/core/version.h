#ifndef OPAL_CORE_VERSION_H_
#define OPAL_CORE_VERSION_H_

#include <string_view>

namespace opal {

// Returns the smithy-cpp runtime version as a semantic version string.
std::string_view Version();

}  // namespace opal

#endif  // OPAL_CORE_VERSION_H_
