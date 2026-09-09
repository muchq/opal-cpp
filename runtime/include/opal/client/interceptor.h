#ifndef OPAL_CLIENT_INTERCEPTOR_H_
#define OPAL_CLIENT_INTERCEPTOR_H_

#include "opal/core/outcome.h"
#include "opal/http/message.h"

namespace opal {

// User-supplied hooks around every HTTP attempt a generated client makes
// (smithy-rs prior art: client interceptors). Register on
// ClientConfig::interceptors; interceptors run in registration order.
// Hooks must not throw — express failures by leaving the request unusable
// for the server (e.g. dropping credentials) rather than raising.
class Interceptor {
 public:
  virtual ~Interceptor() = default;

  // Runs before each attempt (attempt is 1-based; retries see 2, 3, ...).
  // Mutate the outgoing request here: auth headers, tracing ids, ...
  virtual void ModifyBeforeTransmit(http::HttpRequest& request, int attempt) {
    (void)request;
    (void)attempt;
  }

  // Runs after each attempt with the request as sent and the transport
  // outcome (a response of any status, or a transport error). Observe only:
  // logging, metrics, tracing.
  virtual void ReadAfterTransmit(const http::HttpRequest& request,
                                 const Outcome<http::HttpResponse>& outcome, int attempt) {
    (void)request;
    (void)outcome;
    (void)attempt;
  }
};

}  // namespace opal

#endif  // OPAL_CLIENT_INTERCEPTOR_H_
