#include "opal/client/retry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
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
    bodies.push_back(outcome.ok() ? outcome->body : std::string{});
    (void)attempt;
  }

  std::vector<int> modify_attempts;
  std::vector<std::string> seen_headers;
  std::vector<int> statuses;
  std::vector<std::string> bodies;
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

// Issue #213: the one place retries and a body sink disagree.
TEST(SendWithRetriesTest, ARetryableStatusIsNeverOfferedToTheSink) {
  // Streaming the 503's error document and then retrying would leave the sink
  // holding that body followed by the real one, with no way to take the first
  // back. So while another attempt can follow, accept() is not even asked.
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "slow down"},
                      http::HttpResponse{200, {}, "the payload"}};
  std::vector<milliseconds> slept;

  std::string streamed;
  std::vector<int> offered;
  const http::BodySink sink{.accept =
                                [&](int status, const http::Headers&) {
                                  offered.push_back(status);
                                  return true;
                                },
                            .write =
                                [&](std::string_view piece) {
                                  streamed.append(piece);
                                  return true;
                                }};

  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept), {}, sink);
  ASSERT_TRUE(outcome.ok()) << outcome.error().message();
  EXPECT_EQ(transport.calls, 2);
  EXPECT_EQ(streamed, "the payload") << "the sink saw a body it should not have";
  EXPECT_EQ(offered, std::vector<int>{200}) << "the sink was consulted about a retryable status";
  // The buffered 503 was not lost on the way through — it just went where a
  // buffered body goes.
  EXPECT_TRUE(outcome->body.empty());
}

TEST(SendWithRetriesTest, ARetryableStatusIsBufferedEvenWhenItIsTheLastAnswer) {
  // Attempts run out on a 503, so this one is returned rather than discarded
  // — and it still does not reach the sink. The rule does not depend on which
  // attempt produced the status, because a caller should not have to reason
  // about that to know what its sink will be handed. The body is not lost: it
  // arrives where every other error document does.
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "first"}, http::HttpResponse{503, {}, "last"}};
  std::vector<milliseconds> slept;
  RetryPolicy policy = InstantPolicy(&slept);
  policy.max_attempts = 2;

  std::string streamed;
  const http::BodySink sink{.accept = [](int, const http::Headers&) { return true; },
                            .write =
                                [&](std::string_view piece) {
                                  streamed.append(piece);
                                  return true;
                                }};

  const auto outcome = SendWithRetries(transport, {}, policy, {}, sink);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome->status, 503);
  EXPECT_EQ(transport.calls, 2);
  EXPECT_TRUE(streamed.empty());
  EXPECT_EQ(outcome->body, "last");
}

TEST(SendWithRetriesTest, WithRetriesDisabledEveryResponseIsTheCallersToTake) {
  // max_attempts = 1 means no response can be thrown away, so nothing needs
  // withholding and the caller's accept() stands unaltered.
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{503, {}, "the only answer"}};
  RetryPolicy policy;
  policy.max_attempts = 1;

  std::string streamed;
  const http::BodySink sink{.accept = [](int, const http::Headers&) { return true; },
                            .write =
                                [&](std::string_view piece) {
                                  streamed.append(piece);
                                  return true;
                                }};

  const auto outcome = SendWithRetries(transport, {}, policy, {}, sink);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(transport.calls, 1);
  EXPECT_EQ(streamed, "the only answer");
}

TEST(SendWithRetriesTest, AFailureAfterTheSinkTookBytesEndsTheLoop) {
  // A transport that streamed part of a body reports the failure as not
  // retryable (BeastHttpClient does exactly this); the loop must honor that
  // rather than redialing and handing the sink a second copy of the start.
  ScriptedTransport transport;
  transport.script = {Error::Transport("connection reset mid-body", /*retryable=*/false),
                      http::HttpResponse{200, {}, "never reached"}};
  std::vector<milliseconds> slept;

  const http::BodySink sink{.accept = [](int, const http::Headers&) { return true; },
                            .write = [](std::string_view) { return true; }};
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept), {}, sink);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(transport.calls, 1);
  EXPECT_TRUE(slept.empty());
}

TEST(SendWithRetriesTest, InterceptorsSeeAStreamedResponseWithoutItsBody) {
  ScriptedTransport transport;
  transport.script = {http::HttpResponse{200, {}, "to the sink"}};
  auto recorder = std::make_shared<RecordingInterceptor>();

  std::string streamed;
  const http::BodySink sink{.accept = [](int, const http::Headers&) { return true; },
                            .write =
                                [&](std::string_view piece) {
                                  streamed.append(piece);
                                  return true;
                                }};
  const auto outcome = SendWithRetries(transport, {}, RetryPolicy{}, {recorder}, sink);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(streamed, "to the sink");
  EXPECT_EQ(recorder->statuses, std::vector<int>{200});
  ASSERT_EQ(recorder->bodies.size(), 1u);
  EXPECT_TRUE(recorder->bodies[0].empty())
      << "the body went to the sink; a hook cannot see one that was never assembled";
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
