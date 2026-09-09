# ADR-0015: WebSocket transports carry event-stream messages

**Status:** Accepted (2026-07-19). Phase 8 slice 2 of ADR-0014's plan.
Implemented: `opal::http::WebSocket` + `BeastServerTransport` upgrade +
`BeastWebSocketClient` (`//runtime:http_beast`).

## Context

ADR-0014 sliced Phase 8 wire-format-first and landed the event-stream
framing codec as a leaf library. This slice gives those frames a wire:
a WebSocket upgrade path on `BeastServerTransport` (the extension point
ADR-0006 reserved for exactly this) and a blocking client dial — both
speaking `eventstream::Message`, both usable directly by applications
ahead of slice 3's generated `EventStream<Tx, Rx>` API, the same way
consumers already wire raw runtime middleware before codegen support.

The decisions below are transport contracts. Everything about *modeled*
streams — initial-request/initial-response, typed unions, mid-stream
modeled errors — is slice 3's, deliberately not decided here.

## Decisions

- **One event-stream frame per WebSocket binary message.** WebSocket
  framing (RFC 6455) already provides message boundaries, so the codec's
  incremental buffer-accumulation path is not needed on this wire: a
  received binary message must decode to exactly one frame
  (`bytes_consumed == size`); trailing bytes, text messages, and frames
  that fail to decode are protocol errors that fail the session. The
  incremental decode contract stays load-bearing for the future
  HTTP-body event-stream wire (chunked transfer), which has no message
  boundaries of its own.
- **One shared session type, `opal::http::WebSocket`**, for both the
  server-accepted and client-dialed ends: blocking
  `Outcome<std::optional<eventstream::Message>> Receive()` (nullopt is
  the peer's clean close — the stream's natural end, not an error),
  blocking `Outcome<Unit> Send(const eventstream::Message&)`, and
  idempotent `Close()`. Full duplex: one thread may block in `Receive`
  while another blocks in `Send` — mirroring WebSocket's own
  one-read + one-write concurrency, and the shape slice 3's
  sender/receiver pair needs underneath it.
- **Async pumps inside, blocking facade outside.** Beast's synchronous
  websocket read may write control-frame replies, so two threads doing
  sync read + sync write race; the session therefore runs asio async
  operations on the connection's strand and the blocking calls wait on
  them. Backpressure is real on both sides: `Send` returns when its
  frame is on the wire (nothing queues unboundedly), and the receive
  side buffers a small fixed number of messages (an internal constant,
  not a knob) before it stops reading. A paused reader also pauses the
  keep-alive machinery (pings are answered from inside reads), so a
  receiver must come back for its messages within the idle timeout —
  a consumer that stops consuming is eventually indistinguishable from
  a vanished one, and the session ends on the idle deadline.
  Cancellation is `Close()` (a blocked `Receive` unblocks into the
  clean nullopt end) or `Stop()` (blocked calls fail with a transport
  error).
- **The upgrade is offered after the request is fully read.** Requests
  that ask for an upgrade flow through the existing read path first, so
  the decision sees the complete request (target, headers, auth
  material) with the transport's header limits already enforced. Two
  Options fields, both optional-by-default and composable with existing
  middleware policy:
  `websocket_gate` (return an `HttpResponse` to refuse — written as the
  plain HTTP answer, before any 101 exists; unset accepts) and
  `on_websocket` (the serve callback, on the handler pool — its return
  ends the session with a close handshake). With `on_websocket` unset,
  upgrade requests remain ordinary HTTP requests for the handler chain
  to answer (today's behavior, unchanged). Because the serve callback
  blocks by design, `Start` refuses `on_websocket` with
  `handler_threads == 0`.
- **Upgraded connections leave HTTP timeouts behind.** The per-phase
  `request_timeout_seconds` contract (ADR-0013) governs the wire up
  through the 101; after it, Beast's websocket timeout takes over:
  `websocket_idle_timeout_seconds` (default 300, Beast's suggested
  server setting) with keep-alive pings on, so a healthy-but-quiet
  stream stays up and a vanished peer is detected without any
  application ping protocol. The message-size ceiling is the codec's
  own `kMaxMessageBytes` — the transport refuses what the codec could
  not represent, extending ADR-0014's symmetric-bounds line to the
  wire.
- **Connection events extend, not expand.** A new
  `ConnectionEvent::Kind::kUpgradeFailure` reports handshakes that
  failed after a request asked for an upgrade (ADR-0014 anticipated
  this). Once the serve callback runs, wire failures surface through
  `Send`/`Receive` to the application — its serve loop is the observer,
  so the transport stays silent (ADR-0013's silence-means-healthy,
  applied as silence-means-the-app-knows). Gate refusals are served
  HTTP responses, not events.
- **Client dial reuses the ADR-0007 posture wholesale**: TLS 1.2 floor,
  certificate + hostname verification on by default, SNI. A dialed
  connection owns one background io thread for its pumps — long-lived
  streams are few, and a shared-reactor design can arrive with the
  eventual coroutine API without changing this surface. Each handshake
  phase (connect, TLS, upgrade) runs under the `handshake_timeout_ms`
  budget; name resolution uses the system resolver's own timeout.
- **`Stop()` aborts live sessions before the io context stops.** The
  transport keeps a registry of live server sessions; `Stop` fails
  their blocked `Send`/`Receive` calls and closes their sockets first,
  so serve callbacks return and the handler pool joins — a stream
  session never counts toward the request drain (it is not a request
  awaiting a response) and cannot wedge shutdown.

## Alternatives rejected

- **Sync websocket streams with a mutex**: the control-frame write
  inside sync reads makes true full duplex unsafe, and serializing
  Send behind Receive would deadlock the common server pattern (block
  in Receive, push from another thread).
- **A raw-bytes WebSocket API** (send/receive `std::string`): every
  consumer would immediately reimplement the frame mapping; the typed
  session keeps the invariant "what travels is event-stream messages"
  in one place. Slice 3 needs exactly this surface underneath.
- **Accept-then-gate** (complete the 101, let the app close): an
  unauthenticated peer must be refusable with an HTTP status before
  any upgrade exists; a gate that runs after the handshake cannot say
  401.
- **Counting stream sessions in the drain**: `active` drains requests
  whose responses are pending; a stream has no pending response and
  would hold `Stop()` for the full drain timeout every time.

## Consequences

- `//runtime:http_beast` gains a dependency on `//runtime:eventstream`
  (a leaf on `:core` — no new externals) and the websocket headers of
  the same `boost.beast` module (no `beast_src.cc` changes: the BCR
  module compiles Beast's own sources).
- Applications can serve and consume event streams today with ~20
  lines of transport wiring; slice 3 replaces that wiring with
  generated signatures, not this transport layer.
- Definition of done includes the out-of-tree consumer e2e pinned by
  ADR-0014's amendment: a consumer dials the upgraded server and
  drains real frames through the module boundary, in this same PR.
- The one-io-thread-per-dialed-connection cost is accepted and
  documented; revisiting it belongs to the coroutine-API decision, not
  to slice 3.
- Two seams are deliberately reserved rather than built: a close-code
  accessor on `WebSocket` (today the close code travels the wire —
  peers can assert on it — but the local facade reports only
  clean-versus-error; surfacing the code and reason is an additive
  method when a consumer needs it), and an accept-decorator seam on the
  server upgrade (subprotocol negotiation / extra 101 headers via
  Beast's response decorator; the gate sees the request today, but
  nothing can shape the acceptance response). Neither changes the
  session contract. *(Amendment, 2026-07-20: ADR-0018 consumed the
  accept-decorator seam for the negotiated JSON-text frame mode; the
  close-code accessor stays reserved.)*
- The async-adapter pressure point, named: the serve callback's
  borrow-until-return contract (`WebSocket&` valid only while
  `on_websocket` runs) is what a future async/coroutine adapter will
  strain against, since completion-driven code wants sessions that
  outlive their callback. The internals are already
  `shared_ptr`-owned, so lifting the borrow to shared ownership is an
  additive API change, not a rework.
