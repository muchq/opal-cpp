#ifndef OPAL_HTTP_LOOPBACK_H_
#define OPAL_HTTP_LOOPBACK_H_

#include <utility>

#include "opal/http/server_dispatch.h"
#include "opal/http/transport.h"

namespace opal::http {

// In-memory transport: an HttpClient wired directly to a request handler with
// no sockets, serialization of the connection, or threads. The backbone of
// fast client<->server integration tests (PLAN Phase 5) — the same test body
// runs against Loopback and a real socket transport. A test may stamp
// HttpRequest::peer_address to exercise peer-dependent handlers (there is no
// connection to stamp it from).
class Loopback : public HttpClient, public HttpServerTransport {
 public:
  // HttpServerTransport:
  Outcome<Unit> Start(RequestHandler handler) override {
    handler_ = std::move(handler);
    return Unit{};
  }
  void Stop() override { handler_ = nullptr; }

  // HttpClient:
  Outcome<HttpResponse> Send(const HttpRequest& request) override {
    if (!handler_) return Error::Transport("loopback: no handler installed", /*retryable=*/false);
    return InvokeHandlerGuarded(handler_, request);
  }

 private:
  RequestHandler handler_;
};

}  // namespace opal::http

#endif  // OPAL_HTTP_LOOPBACK_H_
