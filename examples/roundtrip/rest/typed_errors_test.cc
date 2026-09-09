// Typed dispatch over an operation's modeled errors (issue #49):
// <Op>Errors::FromError() replaces string-matching Error::code(). Pinned
// against the roundtrip rest golden, whose DescribeSink carries two modeled
// errors — including DescribeSinkError, the operation-named collision
// fixture the plural listing name exists to dodge. The wire half (server
// error → client opal::Error with code + typed detail) is already pinned
// by the generated integration tests; this suite pins the typed view over
// that Error.

#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "example/roundtrip/rest/client.h"
#include "smithy/core/error.h"
#include "smithy/core/overloaded.h"

namespace example::roundtrip::rest {
namespace {

opal::Error SinkNotFoundError() {
  opal::Error error = opal::Error::Modeled("SinkNotFound", "no sink: s1");
  error.set_detail(SinkNotFound{.message = "no sink: s1", .resourceType = "sink"});
  return error;
}

TEST(TypedErrorsTest, MatchesTheModeledErrorAndCarriesTheDetail) {
  const DescribeSinkErrors typed = DescribeSinkErrors::FromError(SinkNotFoundError());
  ASSERT_TRUE(typed.is_sink_not_found());
  EXPECT_FALSE(typed.empty());
  EXPECT_STREQ(typed.case_name(), "sink_not_found");
  EXPECT_EQ(typed.as_sink_not_found().message, "no sink: s1");
  EXPECT_EQ(typed.as_sink_not_found().resourceType, "sink");
  EXPECT_EQ(typed.as_describe_sink_error_or_null(), nullptr);
}

TEST(TypedErrorsTest, DetailLessCodeMatchStillDispatchesWithDefaultFields) {
  const auto typed =
      DescribeSinkErrors::FromError(opal::Error::Modeled("DescribeSinkError", "gone"));
  ASSERT_TRUE(typed.is_describe_sink_error());
  EXPECT_EQ(typed.as_describe_sink_error().message, "");
}

TEST(TypedErrorsTest, OtherCodesAndOtherKindsStayEmpty) {
  // Another operation's modeled error: CreateSink's quota error is not in
  // DescribeSink's listing.
  EXPECT_TRUE(
      DescribeSinkErrors::FromError(opal::Error::Modeled("SinkQuotaExceeded", "full")).empty());
  // The kind gates matching: a non-modeled error never engages a member,
  // even with a matching code string.
  EXPECT_TRUE(DescribeSinkErrors::FromError(
                  opal::Error(opal::ErrorKind::kTransport, "SinkNotFound", "spoofed"))
                  .empty());
  EXPECT_TRUE(DescribeSinkErrors::FromError(opal::Error::Transport("boom")).empty());
}

TEST(TypedErrorsTest, VisitDispatchesExhaustivelyIncludingTheEmptyState) {
  const auto describe = opal::Overloaded{
      [](const SinkNotFound& e) { return "not-found:" + e.message; },
      [](const DescribeSinkError&) { return std::string("gone"); },
      [](std::monostate) { return std::string("unmodeled"); },
  };
  EXPECT_EQ(DescribeSinkErrors::FromError(SinkNotFoundError()).visit(describe),
            "not-found:no sink: s1");
  EXPECT_EQ(DescribeSinkErrors::FromError(opal::Error::Transport("boom")).visit(describe),
            "unmodeled");
}

TEST(TypedErrorsDeathTest, WrongCaseAccessDiesWithContext) {
  const DescribeSinkErrors typed = DescribeSinkErrors::FromError(SinkNotFoundError());
  EXPECT_DEATH((void)typed.as_describe_sink_error(),
               "DescribeSinkErrors::as_describe_sink_error\\(\\): engaged member is"
               " sink_not_found");
}

}  // namespace
}  // namespace example::roundtrip::rest
