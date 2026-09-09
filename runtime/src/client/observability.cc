#include "smithy/client/observability.h"

#include <utility>

namespace opal {
namespace {

class AttemptObserver final : public Interceptor {
 public:
  explicit AttemptObserver(std::function<void(const AttemptObservation&)> callback)
      : callback_(std::move(callback)) {}

  void ReadAfterTransmit(const http::HttpRequest& request,
                         const Outcome<http::HttpResponse>& outcome, int attempt) override {
    AttemptObservation observation;
    observation.method = request.method;
    observation.target = request.target;
    observation.attempt = attempt;
    if (outcome.ok()) {
      observation.status = outcome->status;
    } else {
      observation.status = -1;
      observation.error_message = outcome.error().message();
    }
    callback_(observation);
  }

 private:
  std::function<void(const AttemptObservation&)> callback_;
};

class TracePropagator final : public Interceptor {
 public:
  explicit TracePropagator(std::function<std::optional<http::TraceContext>()> current)
      : current_(std::move(current)) {}

  void ModifyBeforeTransmit(http::HttpRequest& request, int attempt) override {
    (void)attempt;
    if (request.headers.Get("traceparent").has_value()) {
      return;  // Explicitly set by the caller or another interceptor.
    }
    std::optional<http::TraceContext> context;
    if (current_ != nullptr) {
      context = current_();
    }
    if (!context.has_value()) {
      context = http::GenerateTraceContext();
    }
    request.headers.Set("traceparent", http::FormatTraceparent(*context));
  }

 private:
  std::function<std::optional<http::TraceContext>()> current_;
};

}  // namespace

std::shared_ptr<Interceptor> ObserveAttempts(
    std::function<void(const AttemptObservation&)> callback) {
  return std::make_shared<AttemptObserver>(std::move(callback));
}

std::shared_ptr<Interceptor> PropagateTraceContext(
    std::function<std::optional<http::TraceContext>()> current) {
  return std::make_shared<TracePropagator>(std::move(current));
}

}  // namespace opal
