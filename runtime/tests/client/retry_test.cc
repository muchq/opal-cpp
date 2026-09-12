#include "opal/client/retry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "opal/core/error.h"
#include "opal/core/timestamp.h"
#include "opal/http/headers.h"

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

// A response carrying the server's own idea of when to come back.
http::HttpResponse Throttled(int status, const std::string& retry_after) {
  http::HttpResponse response;
  response.status = status;
  if (!retry_after.empty()) response.headers.Set("retry-after", retry_after);
  response.body = "slow down";
  return response;
}

Timestamp At(const char* http_date) {
  auto parsed = Timestamp::Parse(http_date, TimestampFormat::kHttpDate);
  return parsed.ok() ? *parsed : Timestamp{};
}

TEST(RetryAfterDelayTest, ReadsDeltaSeconds) {
  http::Headers headers;
  headers.Set("retry-after", "30");
  EXPECT_EQ(RetryAfterDelay(headers, Timestamp{}), milliseconds(30000));

  headers.Set("retry-after", "0");
  EXPECT_EQ(RetryAfterDelay(headers, Timestamp{}), milliseconds(0));
}

TEST(RetryAfterDelayTest, ReadsAnHttpDateAgainstNow) {
  const Timestamp now = At("Fri, 31 Dec 1999 23:59:00 GMT");
  http::Headers headers;
  headers.Set("retry-after", "Fri, 31 Dec 1999 23:59:30 GMT");
  EXPECT_EQ(RetryAfterDelay(headers, now), milliseconds(30000));

  // Already past: the server is asking for nothing, not for negative time.
  headers.Set("retry-after", "Fri, 31 Dec 1999 23:58:00 GMT");
  EXPECT_EQ(RetryAfterDelay(headers, now), milliseconds(0));
}

TEST(RetryAfterDelayTest, ReadsTheObsoleteHttpDateFormatsToo) {
  // RFC 9110 §5.6.7 requires a recipient to accept all three HTTP-date
  // formats, and Retry-After's HTTP-date alternative inherits that. Ignoring
  // the obsolete two means coming back earlier than a server asked, silently
  // — the bug this whole change exists to fix, just for older servers.
  const Timestamp now = At("Sun, 06 Nov 1994 08:49:00 GMT");
  for (const char* value : {"Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT",
                            "Sun Nov  6 08:49:37 1994"}) {
    http::Headers headers;
    headers.Set("retry-after", value);
    EXPECT_EQ(RetryAfterDelay(headers, now), milliseconds(37000)) << "value: " << value;
  }
}

TEST(RetryAfterDelayTest, AnExtremeReferenceDoesNotOverflowTheSubtraction) {
  // The function is public, and Timestamp::FromEpochMilliseconds is an
  // unchecked factory, so the difference of two legal Timestamps can exceed
  // int64. Signed overflow is undefined behavior, not a large number.
  http::Headers headers;
  headers.Set("retry-after", "Fri, 31 Dec 9999 23:59:59 GMT");
  const auto delay = RetryAfterDelay(
      headers, Timestamp::FromEpochMilliseconds(std::numeric_limits<int64_t>::min()));
  ASSERT_TRUE(delay.has_value());
  EXPECT_GT(*delay, milliseconds(0)) << "a far-future date read as no delay at all";
  EXPECT_GE(*delay, milliseconds(86400000));

  // The mirror image: a far-past date against a far-future reference is zero,
  // not a wrapped positive.
  headers.Set("retry-after", "Thu, 01 Jan 1970 00:00:00 GMT");
  EXPECT_EQ(RetryAfterDelay(headers,
                            Timestamp::FromEpochMilliseconds(std::numeric_limits<int64_t>::max())),
            milliseconds(0));
}

TEST(RetryAfterDelayTest, AbsentOrMalformedAsksForNothing) {
  const http::Headers none;
  EXPECT_EQ(RetryAfterDelay(none, Timestamp{}), std::nullopt);

  // A peer's malformed hint is not worth failing a call over; ordinary
  // backoff is the safe answer. Note "30s" and "+30": near-misses that a lax
  // parser would accept and RFC 9110 does not.
  for (const char* value : {"", "soon", "-5", "30s", "+30", "0x10", " 30", "30 ", "3.5"}) {
    http::Headers headers;
    headers.Set("retry-after", value);
    EXPECT_EQ(RetryAfterDelay(headers, Timestamp{}), std::nullopt) << "value: " << value;
  }
}

TEST(RetryAfterDelayTest, AnAbsurdDelaySaturatesRatherThanOverflowing) {
  // Digits are syntactically fine however many there are. The cap is what
  // makes this harmless, so the parse must not wrap into a small or negative
  // duration on the way there.
  http::Headers headers;
  headers.Set("retry-after", "999999999999999999999");
  const auto delay = RetryAfterDelay(headers, Timestamp{});
  ASSERT_TRUE(delay.has_value());
  EXPECT_GT(*delay, milliseconds(0));
  EXPECT_GE(*delay, milliseconds(86400000));
}

TEST(SendWithRetriesTest, RetryAfterRaisesTheBackoffItDoesNotLowerIt) {
  // The header is a floor under this client's own backoff, per RFC 9110:
  // the server may ask it to wait longer, never to come back sooner.
  ScriptedTransport transport;
  transport.script = {Throttled(429, "2"), http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  const auto outcome = SendWithRetries(transport, {}, InstantPolicy(&slept));
  ASSERT_TRUE(outcome.ok()) << outcome.error().message();
  ASSERT_EQ(slept.size(), 1u);
  EXPECT_EQ(slept[0], milliseconds(2000)) << "the server asked for 2s and got the 100ms backoff";

  // Asking for less than the backoff changes nothing: the backoff is already
  // the longer of the two, and coming back early is what it exists to stop.
  ScriptedTransport impatient;
  impatient.script = {Throttled(503, "0"), http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> impatient_slept;
  ASSERT_TRUE(SendWithRetries(impatient, {}, InstantPolicy(&impatient_slept)).ok());
  ASSERT_EQ(impatient_slept.size(), 1u);
  EXPECT_EQ(impatient_slept[0], milliseconds(100));
}

TEST(SendWithRetriesTest, ClampsRetryAfterToItsOwnCap) {
  // How far this client will trust a number the peer sent. An hour is not
  // an offer a caller has to accept.
  ScriptedTransport transport;
  transport.script = {Throttled(429, "3600"), http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  RetryPolicy policy = InstantPolicy(&slept);
  policy.retry_after_cap = milliseconds(5000);
  ASSERT_TRUE(SendWithRetries(transport, {}, policy).ok());
  ASSERT_EQ(slept.size(), 1u);
  EXPECT_EQ(slept[0], milliseconds(5000));
}

TEST(SendWithRetriesTest, AMalformedOrAbsentRetryAfterLeavesTheBackoffAlone) {
  ScriptedTransport transport;
  transport.script = {Throttled(429, "whenever"), Throttled(503, ""),
                      http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  ASSERT_TRUE(SendWithRetries(transport, {}, InstantPolicy(&slept)).ok());
  EXPECT_EQ(slept, (std::vector<milliseconds>{milliseconds(100), milliseconds(200)}));
}

TEST(SendWithRetriesTest, ATransportErrorHasNoHeaderToHonor) {
  ScriptedTransport transport;
  transport.script = {Error::Transport("refused"), http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  ASSERT_TRUE(SendWithRetries(transport, {}, InstantPolicy(&slept)).ok());
  EXPECT_EQ(slept, std::vector<milliseconds>{milliseconds(100)});
}

TEST(SendWithRetriesTest, AnHttpDateRetryAfterIsMeasuredAgainstTheRealClock) {
  // The loop reads the date form against the wall clock, which the pure
  // parser tests above cannot pin because they supply `now` themselves.
  const auto in_two_seconds =
      Timestamp::FromEpochMilliseconds(std::chrono::duration_cast<milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count() +
                                       2000)
          .Format(TimestampFormat::kHttpDate);

  ScriptedTransport transport;
  transport.script = {Throttled(429, in_two_seconds), http::HttpResponse{200, {}, "ok"}};
  std::vector<milliseconds> slept;
  ASSERT_TRUE(SendWithRetries(transport, {}, InstantPolicy(&slept)).ok());
  ASSERT_EQ(slept.size(), 1u);
  // A second of slack either way: the date has whole-second resolution and
  // the clock moves between building the header and reading it.
  EXPECT_GE(slept[0], milliseconds(500));
  EXPECT_LE(slept[0], milliseconds(3000));
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
