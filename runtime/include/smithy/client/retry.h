#ifndef SMITHY_CLIENT_RETRY_H_
#define SMITHY_CLIENT_RETRY_H_

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "smithy/client/interceptor.h"
#include "smithy/core/outcome.h"
#include "smithy/http/message.h"
#include "smithy/http/transport.h"

namespace opal {

// Retry configuration for generated clients: full-jitter exponential backoff
// (retry n waits uniform(0, min(max_backoff, initial_backoff * 2^(n-1)))).
// sleep and jitter are injectable so tests run instantly and deterministically.
struct RetryPolicy {
  // Total tries including the first; 1 disables retries.
  int max_attempts = 3;
  std::chrono::milliseconds initial_backoff{100};
  std::chrono::milliseconds max_backoff{20000};

  // Overrides for tests; null means a real sleep / a thread-local uniform [0,1).
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<double()> jitter;
};

// The full-jitter delay before 1-based retry number `retry`.
std::chrono::milliseconds RetryDelay(const RetryPolicy& policy, int retry, double jitter01);

// True for the HTTP statuses every Smithy SDK treats as transient:
// 429 (throttling) and 500/502/503/504.
bool RetryableStatus(int status);

// Sends through the transport with retries: transport failures flagged
// retryable (connection, timeout) and transient response statuses are
// retried up to policy.max_attempts, sleeping the backoff in between.
// The last outcome — success or not — is returned as-is. Interceptors run
// around every attempt (ModifyBeforeTransmit on a per-attempt copy of the
// request, ReadAfterTransmit on the outcome), in registration order.
Outcome<http::HttpResponse> SendWithRetries(
    http::HttpClient& transport, const http::HttpRequest& request, const RetryPolicy& policy,
    const std::vector<std::shared_ptr<Interceptor>>& interceptors);

inline Outcome<http::HttpResponse> SendWithRetries(http::HttpClient& transport,
                                                   const http::HttpRequest& request,
                                                   const RetryPolicy& policy) {
  return SendWithRetries(transport, request, policy, {});
}

}  // namespace opal

#endif  // SMITHY_CLIENT_RETRY_H_
