#include "opal/client/retry.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <thread>

namespace opal {
namespace {

double UniformJitter() {
  thread_local std::mt19937 engine{std::random_device{}()};
  return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
}

}  // namespace

std::chrono::milliseconds RetryDelay(const RetryPolicy& policy, int retry, double jitter01) {
  // Cap the exponent so the shift below cannot overflow; max_backoff clamps
  // the result long before that anyway.
  const int exponent = std::min(retry - 1, 20);
  const auto ceiling =
      std::min(policy.max_backoff, policy.initial_backoff * (std::int64_t{1} << exponent));
  return std::chrono::milliseconds(
      static_cast<std::int64_t>(static_cast<double>(ceiling.count()) * jitter01));
}

std::optional<std::chrono::milliseconds> RetryAfterDelay(const http::Headers& headers,
                                                         Timestamp now) {
  const auto value = headers.Get("retry-after");
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }

  // delta-seconds. RFC 9110 §10.2.3 spells it as digits and nothing else:
  // strtoull on its own would also take "+30", " 30" and the "3.5" of a
  // client that guessed, so the shape is checked before the value is read.
  if (value->find_first_not_of("0123456789") == std::string::npos) {
    const unsigned long long seconds = std::strtoull(value->c_str(), nullptr, 10);
    // However many digits a peer sends, the result must not wrap into a
    // small — or negative — duration on its way to the cap that makes it
    // harmless.
    constexpr std::int64_t kMaxSeconds = std::numeric_limits<std::int64_t>::max() / 1000;
    if (seconds > static_cast<unsigned long long>(kMaxSeconds)) {
      return std::chrono::milliseconds::max();
    }
    return std::chrono::seconds(static_cast<std::int64_t>(seconds));
  }

  // The other form: an absolute HTTP-date, in any of the three spellings a
  // recipient must accept (RFC 9110 §5.6.7). `now` resolves the obsolete
  // two-digit year, which is the only thing it is used for there.
  const auto when = http::ParseHttpDate(*value, now);
  if (!when.has_value()) {
    return std::nullopt;
  }
  // A date already past asks for nothing, not for negative time. The ordering
  // is checked before the subtraction rather than after it: this function is
  // public and Timestamp's unchecked factory can build instants whose
  // difference exceeds int64, where a signed subtraction is undefined rather
  // than merely large. Once the pair is ordered, the unsigned difference is
  // exact for any two int64 instants.
  if (*when <= now) {
    return std::chrono::milliseconds(0);
  }
  const std::uint64_t ahead = static_cast<std::uint64_t>(when->epoch_milliseconds()) -
                              static_cast<std::uint64_t>(now.epoch_milliseconds());
  constexpr auto kRepresentable =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return std::chrono::milliseconds(static_cast<std::int64_t>(std::min(ahead, kRepresentable)));
}

bool RetryableStatus(int status) {
  return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

namespace {

Timestamp Now() {
  return Timestamp::FromEpochMilliseconds(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
}

// The wait before the next attempt: this client's own backoff, raised to
// whatever the response asked for and clamped to what the policy will trust.
// Raised, never lowered — Retry-After is a minimum (RFC 9110 §10.2.3), and
// coming back sooner than the backoff is the thing the backoff exists to
// prevent.
std::chrono::milliseconds DelayBefore(const RetryPolicy& policy, int retry, double jitter01,
                                      const Outcome<http::HttpResponse>& outcome) {
  const auto backoff = RetryDelay(policy, retry, jitter01);
  if (!outcome.ok()) {
    return backoff;  // a transport error has no headers to ask with
  }
  const auto asked = RetryAfterDelay(outcome->headers, Now());
  if (!asked.has_value()) {
    return backoff;
  }
  return std::max(backoff, std::min(*asked, policy.retry_after_cap));
}

Outcome<http::HttpResponse> SendWithRetriesImpl(
    http::HttpClient& transport, const http::HttpRequest& request, const RetryPolicy& policy,
    const std::vector<std::shared_ptr<Interceptor>>& interceptors, const http::BodySink* sink) {
  const auto sleep = policy.sleep != nullptr ? policy.sleep : [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  const auto jitter = policy.jitter != nullptr ? policy.jitter : UniformJitter;
  const int attempts = std::max(policy.max_attempts, 1);

  // With retries enabled, a retryable status is kept away from the sink (see
  // the header): accept() is never even consulted for one, on any attempt, so
  // a sink takes payloads and never a transient failure's error document.
  // With retries disabled nothing can be discarded, and the caller's own
  // decision stands unaltered.
  http::BodySink guarded;
  if (sink != nullptr) {
    guarded = *sink;
    if (attempts > 1 && guarded.accept != nullptr) {
      guarded.accept = [accept = guarded.accept](int status, const http::Headers& headers) {
        return !RetryableStatus(status) && accept(status, headers);
      };
    }
  }
  const auto send = [&](const http::HttpRequest& outgoing) {
    return sink != nullptr ? transport.SendStreaming(outgoing, guarded) : transport.Send(outgoing);
  };

  // Each attempt mutates a fresh copy, so interceptor edits never accumulate
  // across retries.
  const auto attempt_send = [&](int attempt) -> Outcome<http::HttpResponse> {
    if (interceptors.empty()) {
      return send(request);  // skip the request copy
    }
    http::HttpRequest attempt_request = request;
    for (const auto& interceptor : interceptors) {
      interceptor->ModifyBeforeTransmit(attempt_request, attempt);
    }
    Outcome<http::HttpResponse> outcome = send(attempt_request);
    for (const auto& interceptor : interceptors) {
      interceptor->ReadAfterTransmit(attempt_request, outcome, attempt);
    }
    return outcome;
  };

  Outcome<http::HttpResponse> outcome = attempt_send(1);
  for (int retry = 1; retry < attempts; ++retry) {
    const bool retryable =
        outcome.ok() ? RetryableStatus(outcome->status) : outcome.error().retryable();
    if (!retryable) {
      return outcome;
    }
    sleep(DelayBefore(policy, retry, jitter(), outcome));
    outcome = attempt_send(retry + 1);
  }
  return outcome;
}

}  // namespace

Outcome<http::HttpResponse> SendWithRetries(
    http::HttpClient& transport, const http::HttpRequest& request, const RetryPolicy& policy,
    const std::vector<std::shared_ptr<Interceptor>>& interceptors) {
  return SendWithRetriesImpl(transport, request, policy, interceptors, nullptr);
}

Outcome<http::HttpResponse> SendWithRetries(
    http::HttpClient& transport, const http::HttpRequest& request, const RetryPolicy& policy,
    const std::vector<std::shared_ptr<Interceptor>>& interceptors, const http::BodySink& sink) {
  return SendWithRetriesImpl(transport, request, policy, interceptors, &sink);
}

}  // namespace opal
