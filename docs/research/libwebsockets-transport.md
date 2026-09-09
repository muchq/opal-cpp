# Research: libwebsockets as an alternative transport

**Status:** Research note (2026-07-25). No decision — input for a future ADR if a concrete
driver appears. Evaluates [libwebsockets](https://libwebsockets.org) (lws) and its
[Bazel Central Registry module](https://registry.bazel.build/modules/libwebsockets) against
the transport contracts pinned by ADR-0006/0007/0013/0015.

## TL;DR

libwebsockets is a credible, actively maintained, MIT-licensed C library that covers
everything the Beast stack does today plus HTTP/2, HTTP/3/WebTransport, WebSocket-over-h2,
client proxy support, and a shared-reactor scaling model. Two hard problems make it
unattractive as a near-term move:

1. **TLS stack split.** The BCR module hard-codes `@openssl` behind a default-off
   `with_tls` flag; this repo pins BoringSSL (ADR-0007). Both export the `SSL_*`/`CRYPTO_*`
   symbol space, so one binary must never link both — and the flag has exactly the
   every-consumer-must-set-it shape ADR-0007 rejected for `boost.asio`'s SSL flag.
2. **Inversion of control.** lws is a single-threaded, callback-driven event loop (only
   `lws_cancel_service()` may be called cross-thread). Every blocking contract this runtime
   promises — `HttpClient::Send`, the full-duplex `WebSocket::Send`/`Receive` pair, the
   handler-pool serve callback — would live behind a hand-built condvar bridge to a service
   thread. That is a full rewrite of the ~2,250-line Beast transport TU with a harder
   concurrency story, re-proven under the TSan suites CI just gained.

**Recommendation: no change now.** The transport abstraction (ADR-0005's dep-light rule)
makes an lws transport purely additive later — `//runtime:http_lws` beside
`//runtime:http_beast`, selected by injection, no generated-code impact. Revisit when a
consumer need materializes that Beast cannot meet (HTTP/2+ wires, event-loop-native
clients, embedded/footprint-constrained targets). Concrete adoption steps are at the end.

## What the transport layer promises today

Generated code targets `HttpClient` / `HttpServerTransport` / `WebSocket` interfaces
(`runtime/include/opal/http/transport.h`, `websocket.h`); Boost.Beast provides the
production implementations in one TU (`runtime/src/http/beast_transport.cc`, ~2,250 lines,
plus the WebSocket session), with TLS via asio-SSL compiled directly against BoringSSL
(`beast_src.cc`, ADR-0007). The contracts an alternative transport must reproduce:

- **Client** (ADR-0007): blocking `Send()` with a real `request_timeout_ms` (async ops
  driven to completion), mutex-guarded idle connection pool, one transparent redial when a
  pooled connection died between requests, TLS 1.2 floor with certificate + hostname
  verification on by default, `ca_pem` / `verify_peer` knobs shared with `ClientConfig`.
- **Server** (ADR-0006/0013): per-phase `request_timeout_seconds`, graceful drain on
  `Stop()`, `ConnectionEvent` reporting with silence-means-healthy semantics, TLS
  termination from PEM text.
- **WebSocket** (ADR-0015): one shared session type for both ends; blocking full-duplex
  `Send`/`Receive` from two threads; `Send` returns when the frame is on the wire; bounded
  receive buffer that stops reading when full; idle timeout with keep-alive pings; upgrade
  offered only after the request is fully read, refusable with a plain HTTP response
  (`websocket_gate`); subprotocol negotiation on the accept (ADR-0018); `Stop()` aborts
  live sessions ahead of the drain.

## What libwebsockets is

- **Scope.** Client and server for HTTP/1.x, HTTP/2, HTTP/3, WebSocket (including
  WS-over-h2/h3), WebTransport, raw TCP/UDP, and MQTT (client), organized as "roles" that
  compile in independently. Far broader than Beast (HTTP/1.1 + WS only).
- **Architecture.** A nonblocking event loop (default `poll()`; glib/libuv/libevent/libev
  supported) delivering C callbacks per connection. Strictly single-threaded per service
  context: all lws API calls must happen on the service thread, with `lws_cancel_service()`
  as the sole documented cross-thread call (it wakes the loop and fires
  `LWS_CALLBACK_EVENT_WAIT_CANCELLED`, from which marshaled work runs). Scales by running
  N service threads with connections spread across them — a shared reactor, versus the
  current one-io-thread-per-dialed-WS-connection and per-connection client `io_context`
  design (a cost ADR-0015 explicitly accepted and deferred to the coroutine-API decision).
- **TLS.** Nine pluggable backends upstream — OpenSSL, BoringSSL, AWS-LC, LibreSSL,
  wolfSSL, mbedTLS, GnuTLS, Schannel, BearSSL — so a BoringSSL build is supported by the
  library itself, just not by the BCR module (below).
- **License.** MIT core (since v4.0) with some files under similarly permissive terms
  (BSD, CC0, zlib) — Apache-2.0-compatible; the BCR "Other" designation reflects the mix,
  not a restriction.
- **Maintenance.** Active: v4.5.0 released December 2025, v4.5.4 March 2026; CI across
  ~269 builds on Linux/macOS/Windows/ESP32/OpenBSD. Mature deployment history (it is the
  WS stack in a long tail of embedded and infrastructure software).

## The Bazel Central Registry module

- **Versions:** 4.5.2 is the latest published (~February 2026, tracking upstream closely).
  Maintained by UebelAndre (a prolific BCR/rules maintainer, not upstream warmcat). The
  module is an overlay — BUILD files written for BCR, not shipped by upstream — with
  `rules_cc_autoconf` replacing the CMake feature-detection step. No other BCR module
  depends on it yet, so we would be the first serious consumer of the overlay.
- **Target and features:** one public `cc_library`, `@libwebsockets//:websockets`, with
  roles H1, H2, RAW, and WS compiled in, `poll()` event loop, Linux/macOS platform
  coverage (matches ADR-0008's platform set), Bazel 7–9 support.
- **TLS is a default-off `bool_flag`** (`--@libwebsockets//:with_tls=true`) that selects
  the OpenSSL source set and a **hard-coded `deps = ["@openssl"]`** (3.5.5.bcr.0). There
  is no `label_flag` to substitute a backend. Two consequences:
  - *Symbol collision hazard:* OpenSSL and BoringSSL both export the `SSL_*`/`CRYPTO_*`
    symbol space with incompatible ABIs. Any single binary linking `//runtime:http_beast`
    (BoringSSL) and a TLS-enabled lws target (OpenSSL) is a latent ODR/link disaster. The
    rule would have to be "one TLS stack per binary" — awkward for this repo's own
    client-tests-server integration suites, which link both sides together.
  - *The flag itself:* a build setting on another module can't be set transitively by ours
    short of a Bazel transition; otherwise every consumer sets it on the command line.
    That is precisely the shape ADR-0007 rejected when it bypassed `boost.asio`'s SSL
    flag by compiling the SSL impl in our own TU — an escape hatch that doesn't exist
    here, because the TLS code is lws's own sources gated by its config defines.

## Contract-by-contract fit

Where the lws primitive lines up well, it lines up very well:

| Contract (ADR) | lws primitive | Fit |
|---|---|---|
| Upgrade gate after full request read, refusable as plain HTTP (0015) | `LWS_CALLBACK_HTTP_CONFIRM_UPGRADE` — reject with a chosen status before the 101 | Good |
| Subprotocol negotiation on accept (0018) | Native per-vhost protocol list | Good |
| Idle timeout + keep-alive pings (0015) | Built-in validity ping/pong machinery | Good |
| Bounded receive buffer / stop reading (0015) | `lws_rx_flow_control()` | Good |
| Close-code accessor (reserved seam, 0015) | Close code/reason exposed in callback | Good |
| Client retry/backoff, proxy, h1 keep-alive reuse (0007's out-of-scope list) | `lws_retry_bo_t`, built-in proxy support, client connection binding/queueing | Better than Beast |
| Blocking `Send`/`Receive` full duplex from two threads (0015) | None — all I/O on the service thread; writes only from the WRITEABLE callback | **Adapter required** |
| Blocking client `Send()` with timeout (0007) | None — same inversion | **Adapter required** |
| Handler pool + drain semantics (0006/0013/0015) | Application-owned; lws only provides the loop | Rebuild |

The adapter is the whole cost. Sketch: one service thread per transport (a shared reactor
— strictly fewer threads than today's per-connection model); `Send` enqueues, calls
`lws_cancel_service()`, requests a WRITEABLE callback, and blocks on a condvar until the
frame is written; `Receive` blocks on a bounded queue the service thread fills, toggling
`lws_rx_flow_control` at the high-water mark. Every contract above then holds — but each
one is our code holding it, where Beast's async-ops-on-a-strand model let the library
carry most of the concurrency reasoning. The TSan CI jobs would be doing load-bearing
work on day one.

## What adoption would buy, and when that matters

- **HTTP/2 (and eventually h3/WebTransport) on both sides** — Beast has no h2 story at
  all. If a protocol or consumer ever needs multiplexed streams over one connection,
  ws-over-h2, or ALPN-negotiated h2, lws is the only realistic C/C++ path short of
  adopting nghttp2 + hand-rolled glue.
- **A shared reactor** — resolves the accepted one-io-thread-per-dialed-connection cost
  of ADR-0015 for fan-out-heavy stream consumers (many concurrent event streams).
- **Foreign event loop integration** (libuv/libevent/glib) — a client embedded in an
  existing event-loop application could integrate natively instead of via our blocking
  facade on background threads. This pressure point is already named in ADR-0015 (the
  async-adapter strain on the borrow-until-return contract).
- **Footprint** — for consumers for whom the Boost + BoringSSL graph is heavy (embedded
  targets are lws's home turf). Note this only pays if lws *replaces* Beast in their
  build, which the dep-light rule permits (transports are opt-in leaf targets).

None of these is a 0.1.0 requirement; all three protocols, the conformance suites, and
the event-stream stack are green on the Beast wire today.

## If pursued: adoption path

1. **Additive, never a swap:** `//runtime:http_lws` implementing the same three
   interfaces, selected by injection like every transport today. Generated code and
   `//runtime:client`/`:core` stay lws-free (ADR-0005 dep-light rule). Beast remains the
   documented default.
2. **Resolve TLS first, in the open:** contribute a `label_flag` (default `@openssl`) to
   the BCR overlay so the backend is substitutable, then point it at `@boringssl` —
   upstream lws supports BoringSSL, so this is overlay work, not a fork of lws. Do not
   ship an lws transport linking OpenSSL into a repo that pins BoringSSL.
3. **Prove the adapter under the existing harness:** the conformance suites, the
   client-tests-server integration matrix, `InMemoryWebSocketPair` parity tests, and the
   TSan jobs are the acceptance bar — the same bar the Beast WebSocket work just cleared.
4. **Write the ADR then**, superseding nothing: ADR-0006/0007/0015 contracts stay
   normative; the ADR would pin the adapter's concurrency design and the one-TLS-stack
   rule.

## Sources

- [libwebsockets.org](https://libwebsockets.org) — roles, event-loop model, TLS backend
  matrix, license statement
- [BCR module page](https://registry.bazel.build/modules/libwebsockets) and
  [module source](https://github.com/bazelbuild/bazel-central-registry/tree/main/modules/libwebsockets/4.5.2)
  (`MODULE.bazel`, `overlay/BUILD.bazel`) — versions, deps, `with_tls` wiring
- [warmcat/libwebsockets releases](https://github.com/warmcat/libwebsockets/releases) —
  4.5.x cadence
- [lws-service.h](https://github.com/warmcat/libwebsockets/blob/main/include/libwebsockets/lws-service.h)
  and [coding notes](https://libwebsockets.org/lws-api-doc-main/html/md_READMEs_README_coding.html)
  — threading rules, `lws_cancel_service()` semantics
- [LICENSE](https://github.com/warmcat/libwebsockets/blob/main/LICENSE) — MIT + permissive mix
