#include "opal/client/retry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "opal/core/error.h"

namespace opal {
namespace {

using std::chrono::milliseconds;

// Scripted transport: pops one outcome per Send.
class ScriptedTransport final : public http::HttpClient {
 public:
  Outcome<http::HttpResponse> Send(const http::HttpRequest& request) override {
    (void)request;
    ++calls;
    if (script.empty()) return http::HttpResponse{200, {}, "fallback"};
    auto next = script.front();
    script.erase(script.begin());
    return next;
  }

  std::vector<Outcome<http::HttpResponse>> script;
  int calls = 0;
};

RetryPolicy InstantPolicy(std::vector<milliseconds>* slept) {
  RetryPolicy policy;
  policy.sleep = [slept](milliseconds d) { slept->push_back(d); };
  policy.jitter = [] { return 1.0; };  // deterministic: always the ceiling
  return policy;
}

TEST(RetryDelayTest, FullJitterExponentialWithCap) {
  RetryPolicy policy;
  policy.initial_backoff = milliseconds(100);
  policy.max_backoff = milliseconds(350);
  EXPECT_EQ(RetryDelay(policy, 1, 1.0), milliseconds(100));
  EXPECT_EQ(RetryDelay(policy, 2, 1.0), milliseconds(200));
  EXPECT_EQ(RetryDelay(policy, 3, 1.0), milliseconds(350));   // capped
  EXPECT_EQ(RetryDelay(policy, 2, 0.5), milliseconds(100));   // jitter scales
  EXPECT_EQ(RetryDelay(policy, 40, 1.0), milliseconds(350));  // huge retry: no overflow
}

TEST(RetryableStatusTest, TransientStatusesOnly) {
  for (int status : {429, 500, 502, 503, 504}) EXPECT_TRUE(RetryableStatus(status)) << status;
  for (int status : {200, 201, 204, 400, 403, 404, 501}) {
    EXPECT_FALSE(RetryableStatus(status)) << status;
  }
}

TEST(SendWithRetriesTest, RetriesTransportErrorsThenSucceeds) {
  ScriptedTransport transport;
  transport.script = {Error::Transport("refused"), Error::Transport("refused"),
                      http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->body, "ok");
  EXPECT_EQ(transport.calls, 3);
  EXPECT_EQ(slept, (std::vector<milliseconds>{milliseconds(100), milliseconds(200)}));
}

TEST(SendWithRetriesTest, RetriesTransientStatuses) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "busy"}, http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 200);
  EXPECT_EQ(transport.calls, 2);
}

TEST(SendWithRetriesTest, DoesNotRetryClientErrors) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{404, {}, "nope"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 404);
  EXPECT_EQ(transport.calls, 1);
  EXPECT_TRUE(slept.empty());
}

TEST(SendWithRetriesTest, GivesUpAfterMaxAttempts) {
  ScriptedTransport transport;
  transport.script = {Error::Transport("a"), Error::Transport("b"), Error::Transport("c"),
                      Error::Transport("d")};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().message(), "c");  // the third (last) attempt's failure
  EXPECT_EQ(transport.calls, 3);
}

// Interceptor recording hook calls and stamping a header per attempt.
class RecordingInterceptor final : public Interceptor {
 public:
  void ModifyBeforeTransmit(http::HttpRequest& request, int attempt) override {
    request.headers.Set("x-attempt", std::to_string(attempt));
    modify_attempts.push_back(attempt);
  }
  void ReadAfterTransmit(const http::HttpRequest& request,
                         const Outcome<http::HttpResponse>& outcome, int attempt) override {
    seen_headers.push_back(request.headers.Get("x-attempt").value_or(""));
    statuses.push_back(outcome.ok() ? outcome->status : -1);
    (void)attempt;
  }

  std::vector<int> modify_attempts;
  std::vector<std::string> seen_headers;
  std::vector<int> statuses;
};

TEST(SendWithRetriesTest, InterceptorsRunAroundEveryAttempt) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "busy"}, Error::Transport("refused"),
                      http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  auto interceptor = std::make_shared<RecordingInterceptor>();
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept), {interceptor});

  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(interceptor->modify_attempts, (std::vector<int>{1, 2, 3}));
  // Each attempt mutates a fresh copy: the stamp never carries over.
  EXPECT_EQ(interceptor->seen_headers, (std::vector<std::string>{"1", "2", "3"}));
  EXPECT_EQ(interceptor->statuses, (std::vector<int>{503, -1, 200}));
}

TEST(SendWithRetriesTest, InterceptorMutationsDoNotLeakIntoCallersRequest) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  http::HttpRequest request;
  const auto outcome = SendWithRetries(transport, request, InstantPolicy(&slept),
                                       {std::make_shared<RecordingInterceptor>()});
  ASSERT_TRUE(outcome.ok());
  EXPECT_FALSE(request.headers.Get("x-attempt").has_value());
}

TEST(SendWithRetriesTest, InterceptorsRunInRegistrationOrder) {
  std::vector<milliseconds> slept;

  class AppendingInterceptor final : public Interceptor {
   public:
    explicit AppendingInterceptor(std::string tag) : tag_(std::move(tag)) {}
    void ModifyBeforeTransmit(http::HttpRequest& request, int) override {
      request.headers.Set("x-tags", request.headers.Get("x-tags").value_or("") + tag_);
    }

   private:
    std::string tag_;
  };

  class CapturingTransport final : public http::HttpClient {
   public:
    Outcome<http::HttpResponse> Send(const http::HttpRequest& request) override {
      tags = request.headers.Get("x-tags").value_or("");
      return http::HttpResponse{200, {}, "ok"};
    }
    std::string tags;
  };

  CapturingTransport capturing;
  (void)SendWithRetries(
      capturing, {}, InstantPolicy(&slept),
      {std::make_shared<AppendingInterceptor>("a"), std::make_shared<AppendingInterceptor>("b")});
  EXPECT_EQ(capturing.tags, "ab");
}

TEST(SendWithRetriesTest, MaxAttemptsOneDisablesRetries) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "busy"}};
  std::vector<milliseconds> slept;
  RetryPolicy policy = InstantPolicy(&slept);
  policy.max_attempts = 1;
  const auto outcome = SendWithRetries(transport, {}, policy);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 503);
  EXPECT_EQ(transport.calls, 1);
}

}  // namespace
}  // namespace opal
