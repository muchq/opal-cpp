#ifndef OPAL_HTTP_TRANSPORT_H_
#define OPAL_HTTP_TRANSPORT_H_

#include <functional>
#include <future>
#include <string>
#include <string_view>

#include "opal/core/exception_guard.h"
#include "opal/core/outcome.h"
#include "opal/http/message.h"

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

// The write half of a BodySink, named because callers pass one on its own:
// a generated operation with a @streaming blob response takes a BodyWriter and
// builds the sink around it (issue #213), so the caller never spells out an
// accept() the protocol already decides.
using BodyWriter = std::function<bool(std::string_view piece)>;

// Where a response body goes when the caller does not want it buffered
// (issue #213). Send() holds a whole body in memory before anything decodes
// it, which is the right default for a modeled JSON response and the wrong
// one for a download whose size the model does not bound.
//
//   BodySink to_file{
//       .accept = [](int status, const Headers&) { return status == 200; },
//       .write = [&](std::string_view piece) { return out.write(...).good(); },
//   };
//   auto response = client.SendStreaming(request, to_file);
//
// Both callbacks must be set for a sink to be used; a sink missing either is
// no sink at all and the response is buffered as usual.
struct BodySink {
  // Called once per response, after the status line and headers and before
  // any body byte. True streams the body to `write` and leaves
  // HttpResponse::body empty; false buffers it there as Send() would. Deciding
  // per response is what lets a caller take a 200 and leave a 404's error
  // document where the rest of the client can read it.
  std::function<bool(int status, const Headers& headers)> accept;

  // Called with each piece as it arrives, in order, never empty. The view is
  // valid only for the duration of the call. False aborts the transfer: the
  // send fails and the connection is dropped rather than reused.
  BodyWriter write;
};

// Client-side transport. Implementations: SocketHttpClient (built-in HTTP/1.1
// over TCP), Loopback (in-memory), adapters for libcurl etc. later.
class HttpClient {
 public:
  virtual ~HttpClient() = default;

  virtual Outcome<HttpResponse> Send(const HttpRequest& request) = 0;

  // Send(), with the response body handed to `sink` instead of buffered when
  // the sink accepts the response. On a sink that accepted, the returned
  // response carries the status and headers and an empty body, and a
  // successful return means the body reached the sink whole.
  //
  // The default implementation is correct everywhere and bounded nowhere: it
  // buffers as Send() does and then hands the body over in one piece, so a
  // transport that cannot stream still honors the contract. BeastHttpClient
  // overrides it and never holds an accepted body. Retries interact with
  // this — see SendWithRetries in opal/client/retry.h, which declines to
  // stream a response it may be about to throw away.
  virtual Outcome<HttpResponse> SendStreaming(const HttpRequest& request, const BodySink& sink) {
    auto outcome = Send(request);
    if (!outcome || sink.accept == nullptr || sink.write == nullptr) {
      return outcome;
    }
    // A sink callback is caller code running on the runtime's completion
    // path, so an exception from one is contained rather than let across the
    // Outcome boundary (ADR-0003, opal/core/exception_guard.h) — the same
    // containment BeastHttpClient::SendStreaming gives it, so a throwing sink
    // fails a call identically whichever transport is underneath.
    return opal::internal::Contain(
        [&]() -> Outcome<HttpResponse> {
          if (!sink.accept(outcome->status, outcome->headers)) {
            return outcome;
          }
          if (!outcome->body.empty() && !sink.write(outcome->body)) {
            return Error::Transport("http: the response body sink aborted the transfer",
                                    /*retryable=*/false);
          }
          outcome->body.clear();
          return outcome;
        },
        [](const char* what) -> Outcome<HttpResponse> {
          return Error::Transport(std::string("http: the response body sink threw: ") +
                                      (what != nullptr ? what : "unknown exception"),
                                  /*retryable=*/false);
        });
  }

  // Async convenience; transports with real event loops should override.
  virtual std::future<Outcome<HttpResponse>> SendAsync(HttpRequest request) {
    return std::async(std::launch::async,
                      [this, request = std::move(request)] { return Send(request); });
  }
};

// What a server transport calls for each incoming request. Handlers express
// failures as HTTP responses; a handler that nonetheless throws is contained
// by the transport (see opal/http/server_dispatch.h) as a 500 with a
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

#endif  // OPAL_HTTP_TRANSPORT_H_
