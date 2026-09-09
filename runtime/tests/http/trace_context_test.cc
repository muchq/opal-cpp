#include "opal/http/trace_context.h"

#include <gtest/gtest.h>

#include <string>

namespace opal::http {
namespace {

constexpr char kTrace[] = "0af7651916cd43dd8448eb211c80319c";
constexpr char kParent[] = "b7ad6b7169203331";

TEST(TraceContextTest, ParsesAValidTraceparent) {
  const auto context = ParseTraceparent(std::string("00-") + kTrace + "-" + kParent + "-01");
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->trace_id, kTrace);
  EXPECT_EQ(context->parent_id, kParent);
  EXPECT_TRUE(context->sampled);

  const auto unsampled = ParseTraceparent(std::string("00-") + kTrace + "-" + kParent + "-00");
  ASSERT_TRUE(unsampled.has_value());
  EXPECT_FALSE(unsampled->sampled);
}

TEST(TraceContextTest, AcceptsFutureVersionsWithTrailingFields) {
  const auto context = ParseTraceparent(std::string("01-") + kTrace + "-" + kParent + "-01-extra");
  EXPECT_TRUE(context.has_value());
}

TEST(TraceContextTest, RejectsMalformedValues) {
  EXPECT_FALSE(ParseTraceparent("").has_value());
  EXPECT_FALSE(ParseTraceparent("garbage").has_value());
  EXPECT_FALSE(ParseTraceparent(std::string("00-") + kTrace + "-" + kParent).has_value());
  // Uppercase hex is invalid per the spec.
  EXPECT_FALSE(
      ParseTraceparent(std::string("00-0AF7651916CD43DD8448EB211C80319C-") + kParent + "-01")
          .has_value());
  // All-zero ids are invalid.
  EXPECT_FALSE(
      ParseTraceparent(std::string("00-00000000000000000000000000000000-") + kParent + "-01")
          .has_value());
  EXPECT_FALSE(ParseTraceparent(std::string("00-") + kTrace + "-0000000000000000-01").has_value());
  // Version ff is forbidden.
  EXPECT_FALSE(ParseTraceparent(std::string("ff-") + kTrace + "-" + kParent + "-01").has_value());
  // Version 00 followed by more fields is malformed (no '-' at 55).
  EXPECT_FALSE(ParseTraceparent(std::string("00-") + kTrace + "-" + kParent + "-01x").has_value());
}

TEST(TraceContextTest, FormatRoundTrips) {
  TraceContext context{kTrace, kParent, true};
  EXPECT_EQ(FormatTraceparent(context), std::string("00-") + kTrace + "-" + kParent + "-01");
  const auto parsed = ParseTraceparent(FormatTraceparent(context));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->trace_id, context.trace_id);
  EXPECT_EQ(parsed->parent_id, context.parent_id);
  context.sampled = false;
  EXPECT_EQ(FormatTraceparent(context), std::string("00-") + kTrace + "-" + kParent + "-00");
}

TEST(TraceContextTest, GeneratedContextsAreValidAndDistinct) {
  const TraceContext a = GenerateTraceContext();
  const TraceContext b = GenerateTraceContext();
  EXPECT_TRUE(ParseTraceparent(FormatTraceparent(a)).has_value());
  EXPECT_TRUE(ParseTraceparent(FormatTraceparent(b)).has_value());
  EXPECT_NE(a.trace_id, b.trace_id);
  EXPECT_NE(GenerateSpanId(), GenerateSpanId());
}

}  // namespace
}  // namespace opal::http
