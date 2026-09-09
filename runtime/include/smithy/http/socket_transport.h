#ifndef SMITHY_HTTP_SOCKET_TRANSPORT_H_
#define SMITHY_HTTP_SOCKET_TRANSPORT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "smithy/http/transport.h"

namespace opal::http {

// Built-in dependency-free HTTP/1.1 client over TCP (ADR-0005; demoted to a
// test/reference transport by ADR-0006). One connection per request,
// plaintext only, Connection: close semantics. It remains the zero-dependency
// default a generated client falls back to for plain-http endpoints;
// production clients inject BeastHttpClient (ADR-0007) for keep-alive
// pooling and TLS.
class SocketHttpClient : public HttpClient {
 public:
  SocketHttpClient(std::string host, int port, int timeout_ms = 30000)
      : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

  Outcome<HttpResponse> Send(const HttpRequest& request) override;

 private:
  std::string host_;
  int port_;
  int timeout_ms_;
};

// Built-in dependency-free HTTP/1.1 server over TCP, bound to 127.0.0.1.
// Test-only (ADR-0006): the accept loop serves one connection at a time on a
// single background thread, so one slow peer stalls everyone behind it —
// fine for integration tests, disqualifying for production, which is why
// Start() logs a relegation notice. Production serving is
// BeastServerTransport (concurrency, timeouts, size limits, TLS, drain).
class SocketHttpServer : public HttpServerTransport {
 public:
  // port 0 binds an ephemeral port; read the real one from port() after Start.
  explicit SocketHttpServer(int port = 0) : requested_port_(port) {}
  ~SocketHttpServer() override;

  Outcome<Unit> Start(RequestHandler handler) override;
  void Stop() override;

  int port() const { return bound_port_; }

 private:
  void AcceptLoop();
  // Non-virtual teardown shared by Stop() and the destructor.
  void Shutdown() noexcept;

  int requested_port_ = 0;
  int bound_port_ = 0;
  std::intptr_t listener_ = -1;  // POSIX socket fd (wide type kept for ABI stability)
  RequestHandler handler_;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
};

}  // namespace opal::http

#endif  // SMITHY_HTTP_SOCKET_TRANSPORT_H_
