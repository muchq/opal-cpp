# ADR-0016: Generated event streams — the EventStream API and its wire binding

**Status:** Accepted (2026-07-19). Phase 8 slice 3 of ADR-0014's plan.
Implemented: `opal::eventstream::EventStream<Tx, Rx>` + envelope helpers,
`opal::server::WebSocketRouter`, `opal::http::InMemoryWebSocketPair`,
generated streaming operations (client + server) for `alloy#simpleRestJson`
and `smithy.protocols#rpcv2Cbor`. Amended by ADR-0023: the jsonRpc2
generation-time refusal is dead (the protocol streams natively over
JSON-RPC envelopes), and the reserved `:initial-request` seam is realized
by jsonRpc2's opening call carrying body-bound initial-request members.

## Context

Slices 1 and 2 built the wire bottom-up: the event-stream framing codec
(ADR-0014) and the WebSocket transports that carry it (ADR-0015). This
slice makes `@streaming` event-stream unions real in generated code —
client and handler signatures, the typed session between them, and the
binding convention that says exactly which bytes travel. The API decisions
ADR-0014 recorded as *direction* become contract here: a blocking
sender/receiver pair mirroring the sync unary API, no coroutine surface
before 1.0 (a coroutine adapter can wrap a blocking pair, not vice versa),
backpressure by bounded queue, cancellation by close — all inherited from
the ADR-0015 session underneath rather than re-implemented.

## The wire binding (authored, vendor-neutral)

Neither alloy nor core Smithy defines how these protocols' event streams
ride WebSocket, so opal-cpp authors the convention the way it authored
jsonRpc2 (ADR-0002 precedent: the trait and the generator are the
normative definition, with an in-repo suite):

- **A streaming operation binds to its `@http` method + URI.** The client
  performs a WebSocket upgrade GET on that URI; labels, query, and
  header bindings resolve exactly as for a unary operation. rpcv2Cbor
  streaming operations upgrade on the protocol's fixed
  `/service/{S}/operation/{O}` URI.
- **One event per binary WebSocket message**, in the slice-1 framing.
  The envelope headers follow the event-stream format's own conventions
  (vendor-neutral for the same reason the framing is — they are the
  format's, not a vendor service's):
  - `:message-type` — `"event"` or `"exception"`.
  - `:event-type` — the event union member name (events only).
  - `:exception-type` — the error shape name (exceptions only).
  - `:content-type` — the protocol's content type
    (`application/json` / `application/cbor`).
- **Payloads are the protocol's bytes**: the event structure serialized
  through the same `Serialize<Shape>` → Document → `json::Encode` /
  `cbor::Encode` pivot every unary body uses. The runtime owns the
  envelope (one place mints and parses the headers); generated code owns
  only the member-name dispatch and serde calls.
- **A received exception is terminal**: it surfaces as the receiving
  side's `Outcome` error — `Error::Modeled(code, message)` with the
  typed detail attached, exactly the unary shape — and the stream ends
  with a close. A handler returning an error sends the exception message
  first, then closes.
- **Initial members**: input-structure members outside the stream must be
  bound to labels, query, or headers (they travel on the upgrade
  request). Output-structure members outside the stream — the
  initial-response — are **rejected at generation time** with a clear
  diagnostic, as are unbound (body) initial-request members: the
  blocking handler contract has no seam to produce initial members
  before streaming without inverting message order, so the deferral is
  scope honesty, not a wire constraint. The envelope reserves
  `":initial-response"` for the additive lift later.

## The API

- **`opal::eventstream::EventStream<Tx, Rx>`** (runtime): blocking
  `Outcome<Unit> Send(const Tx&)`,
  `Outcome<std::optional<Rx>> Receive()` (nullopt is the peer's clean
  close — the ADR-0015 convention verbatim), idempotent `Close()`. It
  wraps one `opal::http::WebSocket` plus two codec functions the
  generated code supplies; full-duplex threading, backpressure, and
  cancellation are the session's existing contract.
- **Client**: a streaming operation generates
  `Outcome<EventStream<InEvents, OutEvents>> Op(const OpInput& input)`.
  `Create` wires a WebSocket dialer from the same `ClientConfig`
  endpoint (host, port, TLS options — nothing configured twice);
  `ClientConfig::websocket_dialer` injects a custom one the way
  `http_client` injects the unary transport, which is also how tests run
  streams without Beast.
- **Server**: the handler grows
  `Outcome<Unit> Op(const OpInput& input, EventStream<OutEvents, InEvents>& stream,
  const RequestContext& context)` — input first, context last, the
  ADR-0010 shape. The generated server exposes `StreamRouter()`, a
  `opal::server::WebSocketRouter` with every streaming route
  registered; applications mount it in one line each on
  `websocket_gate` / `on_websocket`. The router reuses the HTTP
  `Router`'s matching (method, literals > labels > greedy) and populates
  the same `RequestContext` from the upgrade request, so routing
  behavior cannot drift between unary and streaming.
- **Transport-neutral seams**: `WebSocketRouter` and `EventStream` speak
  `opal::http::WebSocket`, never Beast. `InMemoryWebSocketPair` (the
  loopback analog) lives in dep-light `:http`, so generated integration
  tests run streams without Boost; the real-wire chat example covers
  Beast in CI and via the g++ path locally.

## Scope decisions

- **simpleRestJson and rpcv2Cbor get streams; jsonRpc2 refuses them** at
  generation time with a diagnostic — its single-POST envelope has no
  per-operation URI to upgrade on, and no event-stream story worth
  inventing until a consumer needs one.
- **`@eventHeader` / `@eventPayload` are rejected for now** with
  diagnostics: all event members ride the payload document. Both traits
  are additive refinements of the envelope this ADR pins.
- **`@streaming` blobs remain unmodeled** (the README limitation
  narrows but does not disappear): event-stream unions are Phase 8's
  substance; unbounded blob bodies are a different transport problem.
- The full-duplex chat fixture (generated client ↔ generated server over
  real WebSockets, per PLAN §Phase 8's exit criterion) plus
  in-memory-pair integration tests land with this slice.

## Alternatives rejected

- **Async/coroutine API now**: recorded in ADR-0014 and unchanged — a
  blocking pair is wrappable later; the reverse locks the ABI.
- **Streams through the HTTP middleware chain**: the upgrade path
  deliberately bypasses it (ADR-0015); a parallel gate/router keeps
  admission policy composable without pretending a session is a request.
- **Body-bound initial-request members via an `":initial-request"`
  message**: deferred, not refused — the envelope reserves the name; the
  generator diagnostic keeps today's honest boundary visible.
- **A `ws://` endpoint scheme in `ClientConfig`**: one endpoint is the
  point; the dial derives from the http(s) endpoint the unary client
  already parses, so a streaming service needs zero extra config.

## Consequences

- Generated BUILD deps grow `//runtime:eventstream` (and, for the
  default dialer, `//runtime:http_beast`) only when a service has
  streaming operations — dep-light unary consumers pay nothing.
- The `GeneratedCodeShapeTest` pin on "@streaming is ignored" flips, and
  the README limitation is rewritten around what remains (blob streams).
- The envelope helpers are the single place the header convention
  lives; slice-3 generated code, future `@eventHeader` support, and any
  future HTTP-chunked event-stream wire all go through them.
- The handler's borrow-until-return contract (the `stream&` parameter is
  valid only until the method returns) is the async adapter's pressure
  point — the same seam ADR-0015 names for the raw session: the
  underlying sessions are already `shared_ptr`-owned, so promoting the
  borrow to shared ownership for a completion-driven handler shape is an
  additive lift, not a rework of this API.
