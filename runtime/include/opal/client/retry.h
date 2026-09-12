#ifndef OPAL_CLIENT_RETRY_H_
#define OPAL_CLIENT_RETRY_H_

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "opal/client/interceptor.h"
#include "opal/core/outcome.h"
#include "opal/core/timestamp.h"
#include "opal/http/headers.h"
#include "opal/http/message.h"
#include "opal/http/transport.h"

namespace opal {

// Retry configuration for generated clients: full-jitter exponential backoff
// (retry n waits uniform(0, min(max_backoff, initial_backoff * 2^(n-1)))).
// sleep and jitter are injectable so tests run instantly and deterministically.
struct RetryPolicy {
  // Total tries including the first; 1 disables retries.
  int max_attempts = 3;
  std::chrono::milliseconds initial_backoff{100};
  std::chrono::milliseconds max_backoff{20000};

  // Ceiling on a delay the *server* asked for via Retry-After (issue #189).
  // Separate from max_backoff because the two bound different things:
  // max_backoff bounds a guess this client made, and this bounds how far it
  // will trust a number the peer sent. It is the larger of the two by
  // default because the header exists to ask for longer than a client would
  // choose — a service documenting "Retry-After: 30" wants 30, and truncating
  // that to max_backoff would spend an attempt arriving early to be refused
  // again. Total wall-clock across attempts is still unbounded here; that is
  // issue #189 item 7.
  std::chrono::milliseconds retry_after_cap{60000};

  // Overrides for tests; null means a real sleep / a thread-local uniform [0,1).
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<double()> jitter;
};

// The full-jitter delay before 1-based retry number `retry`.
std::chrono::milliseconds RetryDelay(const RetryPolicy& policy, int retry, double jitter01);

// True for the HTTP statuses every Smithy SDK treats as transient:
// 429 (throttling) and 500/502/503/504.
bool RetryableStatus(int status);

// The delay a response's Retry-After header asks for, or nullopt when the
// header is absent or does not parse. RFC 9110 §10.2.3 gives it two forms:
// delta-seconds ("30") and an HTTP-date ("Fri, 31 Dec 1999 23:59:59 GMT"),
// the second measured against `now`. A date already past asks for nothing,
// so it reads as zero rather than as negative.
//
// A header that does not parse is ignored rather than fatal: a peer's
// malformed hint is not worth failing a call over, and ordinary backoff is
// a safe answer. SendWithRetries takes the result as a *floor* under its own
// backoff, clamped to RetryPolicy::retry_after_cap — the server can ask this
// client to wait longer, never to come back sooner.
std::optional<std::chrono::milliseconds> RetryAfterDelay(const http::Headers& headers,
                                                         Timestamp now);

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

// The same, with the response body streamed to `sink` rather than buffered
// (issue #213). Retries and streaming disagree about one thing, and this
// settles it: while retries are enabled, a retryable status (429/5xx) is
// buffered into HttpResponse::body and never offered to the sink. Streaming a
// 503's error document and then retrying would leave the sink holding that
// body followed by the real one, with nothing able to take the first back.
//
// The rule is the same on the last attempt as on the first, deliberately: a
// sink takes payloads, never a transient failure's error document, and which
// attempt produced a 503 is not something a caller should have to reason
// about. Nothing is lost either way — such a body is small, and it arrives
// where every other error document already does. With retries disabled
// (max_attempts = 1) no response can be discarded, so the caller's accept()
// stands unaltered for every status.
//
// A failure after the sink has taken bytes is not retryable, so the loop ends
// there whatever the policy says.
Outcome<http::HttpResponse> SendWithRetries(
    http::HttpClient& transport, const http::HttpRequest& request, const RetryPolicy& policy,
    const std::vector<std::shared_ptr<Interceptor>>& interceptors, const http::BodySink& sink);

}  // namespace opal

#endif  // OPAL_CLIENT_RETRY_H_
