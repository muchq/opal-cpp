#include "opal/core/exception_guard.h"

#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

namespace opal::internal {
namespace {

TEST(ContainTest, ReturnsBodyResultWhenNothingThrows) {
  const int value = Contain([] { return 7; }, [](const char*) { return -1; });
  EXPECT_EQ(value, 7);
}

TEST(ContainTest, RunsBodyForItsSideEffects) {
  int calls = 0;
  Contain([&] { ++calls; }, [](const char*) {});
  EXPECT_EQ(calls, 1);
}

TEST(ContainTest, DoesNotInvokeHandlerOnTheHappyPath) {
  bool handled = false;
  const bool ok = Contain([] { return true; },
                          [&](const char*) {
                            handled = true;
                            return false;
                          });
  EXPECT_TRUE(ok);
  EXPECT_FALSE(handled);
}

#if defined(__cpp_exceptions)
// These paths exist only when the translation unit is built with exceptions.
// Under -fno-exceptions the body cannot throw, so the compiler never sees a
// throw here and the whole block is (correctly) excluded.

TEST(ContainTest, RoutesStdExceptionMessageToHandler) {
  std::string seen;
  const int value = Contain([]() -> int { throw std::runtime_error("boom"); },
                            [&](const char* what) {
                              seen = what == nullptr ? "<null>" : what;
                              return -1;
                            });
  EXPECT_EQ(value, -1);
  EXPECT_EQ(seen, "boom");
}

TEST(ContainTest, PassesNullptrForNonStdThrow) {
  bool null_seen = false;
  Contain([] { throw 42; }, [&](const char* what) { null_seen = (what == nullptr); });
  EXPECT_TRUE(null_seen);
}

TEST(ContainTest, HandlerFallbackValueIsReturnedOnThrow) {
  const bool probed = Contain([]() -> bool { throw std::logic_error("probe failed"); },
                              [](const char*) { return false; });
  EXPECT_FALSE(probed);
}
#endif

}  // namespace
}  // namespace opal::internal
