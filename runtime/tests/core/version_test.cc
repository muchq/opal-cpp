#include "opal/core/version.h"

#include <gtest/gtest.h>

namespace opal {
namespace {

TEST(VersionTest, ReturnsSemanticVersion) {
  EXPECT_FALSE(Version().empty());
  EXPECT_EQ(Version(), "0.3.0-dev");
}

}  // namespace
}  // namespace opal
