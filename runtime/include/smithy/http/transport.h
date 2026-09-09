#ifndef SMITHY_HTTP_TRANSPORT_H_
#define SMITHY_HTTP_TRANSPORT_H_

#include <functional>
#include <future>
#include <string>

#include "smithy/core/outcome.h"
#include "smithy/http/message.h"

namespace opal::http {

// TLS verification knobs, defined once for both sides of the handoff:
// opal::ClientConfig carries one (the knob consumers set) and TLS-capable
// client transports embed the same struct (BeastHttpClient::Options), so the
// shape, defaults, and semantics can't drift apart. Certificate + hostname
// verification is on by default. `ca_pem` replaces the system trust roots
// (PEM text, not a file path — private CAs, tests); setting
// `verify_peer = false` disables verification entirely — never do that in
// production.
struct TlsOptions {
  bool verify_peer = true;
  std::string ca_pem{};
};

// Client-side transport. Implementations: SocketHttpClient (built-in HTTP/1.1
// over TCP), Loopback (in-memory), adapters for libcurl etc. later.
class HttpClient {
 public:
  virtual ~HttpClient() = default;

  virtual Outcome<HttpResponse> Send(const HttpRequest& request) = 0;

  // Async convenience; transports with real event loops should override.
  virtual std::future<Outcome<HttpResponse>> SendAsync(HttpRequest request) {
    return std::async(std::launch::async,
                      [this, request = std::move(request)] { return Send(request); });
  }
};

// What a server transport calls for each incoming request. Handlers express
// failures as HTTP responses; a handler that nonetheless throws is contained
// by the transport (see smithy/http/server_dispatch.h) as a 500 with a
// correlation id rather than taking down the process.
using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

// Server-side transport: binds a listener and dispatches requests to a
// handler. Implementations: SocketHttpServer (built-in), Loopback.
class HttpServerTransport {
 public:
  virtual ~HttpServerTransport() = default;

  virtual Outcome<Unit> Start(RequestHandler handler) = 0;
  virtual void Stop() = 0;
};

}  // namespace opal::http

#endif  // SMITHY_HTTP_TRANSPORT_H_
