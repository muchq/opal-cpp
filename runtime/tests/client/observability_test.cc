#include "smithy/client/observability.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "smithy/core/error.h"
#include "smithy/core/outcome.h"
#include "smithy/http/message.h"
#include "smithy/http/trace_context.h"

namespace {

opal::http::HttpRequest MakeRequest() {
  opal::http::HttpRequest request;
  request.method = "POST";
  request.target = "/books?pageSize=10";
  return request;
}

TEST(ObserveAttemptsTest, ReportsSuccessfulAttempts) {
  std::vector<opal::AttemptObservation> seen;
  const auto interceptor =
      opal::ObserveAttempts([&seen](const opal::AttemptObservation& obs) { seen.push_back(obs); });

  opal::http::HttpResponse response;
  response.status = 201;
  interceptor->ReadAfterTransmit(MakeRequest(), opal::Outcome<opal::http::HttpResponse>(response),
                                 1);

  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].method, "POST");
  EXPECT_EQ(seen[0].target, "/books?pageSize=10");
  EXPECT_EQ(seen[0].attempt, 1);
  EXPECT_EQ(seen[0].status, 201);
  EXPECT_TRUE(seen[0].error_message.empty());
}

TEST(ObserveAttemptsTest, ReportsTransportErrorsAsStatusMinusOne) {
  std::vector<opal::AttemptObservation> seen;
  const auto interceptor =
      opal::ObserveAttempts([&seen](const opal::AttemptObservation& obs) { seen.push_back(obs); });

  interceptor->ReadAfterTransmit(
      MakeRequest(),
      opal::Outcome<opal::http::HttpResponse>(opal::Error::Transport("connection refused")), 3);

  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].attempt, 3);
  EXPECT_EQ(seen[0].status, -1);
  EXPECT_EQ(seen[0].error_message, "connection refused");
}

TEST(ObserveAttemptsTest, ObservesEveryAttemptOfARetryLoop) {
  int calls = 0;
  const auto interceptor =
      opal::ObserveAttempts([&calls](const opal::AttemptObservation&) { ++calls; });
  opal::http::HttpResponse throttled;
  throttled.status = 429;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    interceptor->ReadAfterTransmit(MakeRequest(),
                                   opal::Outcome<opal::http::HttpResponse>(throttled), attempt);
  }
  EXPECT_EQ(calls, 3);
}

TEST(PropagateTraceContextTest, StampsAWellFormedTraceparent) {
  const auto interceptor = opal::PropagateTraceContext();
  auto request = MakeRequest();
  interceptor->ModifyBeforeTransmit(request, 1);

  const auto header = request.headers.Get("traceparent");
  ASSERT_TRUE(header.has_value());
  const auto parsed = opal::http::ParseTraceparent(*header);
  ASSERT_TRUE(parsed.has_value()) << *header;
  EXPECT_TRUE(parsed->sampled);
}

TEST(PropagateTraceContextTest, RespectsAnExistingTraceparent) {
  const auto interceptor = opal::PropagateTraceContext();
  auto request = MakeRequest();
  const std::string preset = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";
  request.headers.Set("traceparent", preset);
  interceptor->ModifyBeforeTransmit(request, 1);
  EXPECT_EQ(request.headers.Get("traceparent"), preset);
}

TEST(PropagateTraceContextTest, UsesTheApplicationsCurrentContext) {
  opal::http::TraceContext context;
  context.trace_id = "0af7651916cd43dd8448eb211c80319c";
  context.parent_id = "b7ad6b7169203331";
  context.sampled = true;
  const auto interceptor = opal::PropagateTraceContext(
      [context]() -> std::optional<opal::http::TraceContext> { return context; });

  auto request = MakeRequest();
  interceptor->ModifyBeforeTransmit(request, 1);
  EXPECT_EQ(request.headers.Get("traceparent"),
            "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
}

TEST(PropagateTraceContextTest, FallsBackToAFreshRootWhenTheCallbackHasNoContext) {
  const auto interceptor = opal::PropagateTraceContext(
      []() -> std::optional<opal::http::TraceContext> { return std::nullopt; });

  auto request = MakeRequest();
  interceptor->ModifyBeforeTransmit(request, 1);
  const auto header = request.headers.Get("traceparent");
  ASSERT_TRUE(header.has_value());
  EXPECT_TRUE(opal::http::ParseTraceparent(*header).has_value()) << *header;

  // Each attempt without an application context gets its own root.
  auto second = MakeRequest();
  interceptor->ModifyBeforeTransmit(second, 2);
  EXPECT_NE(request.headers.Get("traceparent"), second.headers.Get("traceparent"));
}

}  // namespace
