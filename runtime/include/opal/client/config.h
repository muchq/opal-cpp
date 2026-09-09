#ifndef OPAL_CLIENT_CONFIG_H_
#define OPAL_CLIENT_CONFIG_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "opal/client/interceptor.h"
#include "opal/client/retry.h"
#include "opal/http/http1.h"
#include "opal/http/transport.h"
#include "opal/http/websocket.h"

namespace opal {

// Configuration shared by every generated client.
//
//   opal::ClientConfig cfg;
//   cfg.endpoint = "http://localhost:8080";
//   WeatherClient client(cfg);
//
// When http_client is left null, generated clients construct the built-in
// socket transport from the endpoint. Tests inject a Loopback or mock here.
struct ClientConfig {
  std::string endpoint;
  int request_timeout_ms = 30000;
  // Mirrors opal::Version() (runtime/src/core/version.cc), the single source
  // of truth for the product version.
  std::string user_agent = "opal-cpp/0.3.0-dev";

  // TLS knobs for transports constructed from this config (issue #49):
  // BeastHttpClient::FromConfig honors them; the built-in socket transport is
  // plaintext-only, and generated Create() rejects https endpoints without an
  // injected transport. Semantics and defaults live on the shared struct
  // (opal/http/transport.h).
  http::TlsOptions tls;

  // Idle keep-alive connections a pooling transport (FromConfig-built)
  // retains for reuse; the built-in socket transport opens one connection
  // per request.
  std::size_t max_idle_connections = 4;

  // The largest response body a transport built from this config will hold
  // (issue #189). Responses are buffered whole before decoding, so this is
  // the memory one call can make the process commit: a body over it fails
  // the call with a non-retryable transport error naming this knob — on the
  // declared Content-Length before any of it is read where the server
  // declares one. Both built-in transports honor it; an injected transport
  // owns its own limit. Size it to the largest response the service can
  // legitimately return, not to the machine.
  std::size_t max_response_bytes = http::kDefaultMaxBodyBytes;

  // Full-jitter exponential backoff for transport failures and transient
  // statuses (429/5xx); retry.max_attempts = 1 disables retries.
  RetryPolicy retry;

  // @requestCompression: bodies at least this large are gzip-compressed
  // (the Smithy default; 0 compresses everything).
  int request_min_compression_size_bytes = 10240;

  // @httpBearerAuth: when set on a service modeled with the trait, every
  // request carries "authorization: Bearer <token>". Called per request, so
  // rotating credentials just works.
  std::function<std::string()> bearer_token;

  // @httpApiKeyAuth: when set on a service modeled with the trait, every
  // request carries the key where the model binds it (named header with
  // optional scheme, or query parameter). Called per request.
  std::function<std::string()> api_key;

  // User-supplied hooks around every HTTP attempt (auth headers, logging,
  // tracing); run in registration order. See opal/client/interceptor.h.
  std::vector<std::shared_ptr<Interceptor>> interceptors;

  // Optional transport override; shared so several clients can reuse one.
  std::shared_ptr<http::HttpClient> http_client;

  // Optional WebSocket dialer override for event-stream operations
  // (ADR-0016), injected the way http_client injects the unary transport —
  // which is also how tests run streams without Beast. When unset, generated
  // streaming clients dial with opal::http::BeastWebSocketClient::Dialer()
  // from this config's endpoint and TLS options.
  http::WebSocketDialer websocket_dialer;
};

}  // namespace opal

#endif  // OPAL_CLIENT_CONFIG_H_
