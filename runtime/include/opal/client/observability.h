#ifndef OPAL_CLIENT_OBSERVABILITY_H_
#define OPAL_CLIENT_OBSERVABILITY_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "opal/client/interceptor.h"
#include "opal/http/trace_context.h"

namespace opal {

// One HTTP attempt made by a generated client, as seen by ObserveAttempts.
struct AttemptObservation {
  std::string method;
  std::string target;
  int attempt = 0;            // 1-based; retries observe 2, 3, ...
  int status = 0;             // HTTP status, or -1 on a transport error
  std::string error_message;  // transport error text; empty otherwise
};

// The client-side logging/metrics hook: an interceptor reporting every
// attempt (count = callbacks, retries visible via attempt > 1). The callback
// runs on the calling thread after each attempt; keep it cheap or hand off.
std::shared_ptr<Interceptor> ObserveAttempts(
    std::function<void(const AttemptObservation&)> callback);

// W3C Trace Context propagation: sets a traceparent header on every attempt
// that does not already carry one. current supplies the application's active
// trace context (e.g. the current span); when null or empty, a fresh root
// context is generated per attempt. Register alongside your other
// interceptors:
//
//   config.interceptors.push_back(opal::PropagateTraceContext());
std::shared_ptr<Interceptor> PropagateTraceContext(
    std::function<std::optional<http::TraceContext>()> current = nullptr);

}  // namespace opal

#endif  // OPAL_CLIENT_OBSERVABILITY_H_
