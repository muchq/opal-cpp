# ADR-0019: Completion-driven event streams — the async seam and the coroutine adapter

**Status:** Accepted (2026-07-20). The async adapter ADR-0014/0016/0017 name
as future work; runtime slice only (generated handler and client surfaces
stay blocking — see Non-goals).
Implemented: `WebSocket::{ReceiveAsync, SendAsync, SupportsAsync}` (native
on the Beast sessions and `InMemoryWebSocketPair`),
`BeastServerTransport::Options::on_websocket_session`,
`opal::eventstream::AsyncEventStream<Tx, Rx>` + `opal::eventstream::Detached`,
`EventStreamHandle::{SendAsync, SupportsAsync}`,
`SessionRegistry Options::async_delivery`,
`WebSocketRouter::{AddSession, ServeSession}` (issue #118, the shared-seam
route mount), the `examples/chat` async hub.

## Context

Every live stream costs two threads today: the serve callback pins a
handler-pool thread for the session's lifetime (ADR-0016's borrow contract
— `on_websocket`'s return ends the session), and a `SessionRegistry` in
async-free form spends a writer thread per session (ADR-0017). With the
default `handler_threads = 16`, a stock server tops out at sixteen
concurrent streams; a hub of N players runs ~2N threads. Meanwhile the
Beast sessions are already completion-driven inside — the blocking
`Receive` is a condition-variable facade over an internal read pump, and
`Send` a facade over `async_write` — so the thread cost is pure facade,
not substance. ADR-0016 recorded the plan: the blocking pair stays the
stable surface, an adapter wraps it later; ADR-0017 built the shared,
revocable session view that an async session needs. This ADR is that
adapter.

## Decision

**Two async primitives on the `WebSocket` seam, one-outstanding each.**
`ReceiveAsync(callback)` and `SendAsync(message, callback)` complete
exactly once with the same outcomes their blocking twins return, on an
unspecified thread (the transport's completion context: a Beast io thread;
for the in-memory pair, whichever peer thread completed the operation —
after all internal locks are released). At most one receive-class and one
send-class operation may be outstanding per session, across the blocking
and async APIs together: a second async call is refused inline with
`Error::Validation`, while blocking callers keep their serialize-by-waiting
behavior. `Close()` (any thread) completes outstanding operations the way
it unblocks the blocking calls: a parked receive gets the clean-end
`nullopt`, a parked send `Error::Transport` — cancellation stays "close
the session", unchanged.
`SupportsAsync()` reports capability; the base-class defaults refuse with
`Error::Validation` and report false, so custom test sockets keep compiling
and every layer above can fall back honestly. Both in-repo transports
implement natively: the Beast session parks the callback where the facade
parked its condition variable (server and dialed client share `WsSession`),
and the pair parks it in the shared state, fired by the peer's next
operation or close.

**A session the serve callback does not have to babysit.**
`BeastServerTransport::Options::on_websocket_session` receives
`(const HttpRequest&, std::shared_ptr<WebSocket>)` and may return
immediately: the session lives until a `Close` (stream, handle, or peer),
the idle timeout, or `Stop()` — which aborts these sessions through the
same weak-registry sweep as borrowed ones (ADR-0015 semantics, unchanged).
At most one of `on_websocket` / `on_websocket_session` may be set;
`websocket_gate` and the ADR-0018 JSON-frames negotiation apply to both.
The callback runs contained on the handler pool, like everything
application-authored. The borrowed seam and the generated serve path are
byte-for-byte untouched.

**A coroutine adapter over the primitives.**
`opal::eventstream::AsyncEventStream<Tx, Rx>` owns the session
(`shared_ptr<WebSocket>`) plus the two codecs and exposes awaitables:
`co_await stream.Receive()` → `Outcome<std::optional<Rx>>` (nullopt is the
clean end; a decode failure or received exception is terminal and closes,
exactly like `EventStream::Receive`), `co_await stream.Send(event)` →
`Outcome<Unit>`, plus `Close()` and `Share()` — the same
`EventStreamHandle` over the same revocable view (ADR-0017), so blocking
sends from other threads and the registry compose unchanged. Coroutines
resume on the completion context: an async handler must not block there;
blocking work belongs on the application's own threads, reached through a
handle. `opal::eventstream::Detached` is the session-loop return type — a
fire-and-forget coroutine whose unhandled exceptions are contained to a log
line, the transport's containment posture. No general task/executor
framework ships: one stream type, two awaitables, one detached launcher is
the whole surface, and the generated-API adapter can grow on it later.

**Async delivery in the registry, per entry, by capability.**
`SessionRegistry Options::async_delivery = true` replaces each writer
thread with a send chain: `Enqueue` starts a delivery when none is in
flight, and each completion sends the next queued event — FIFO, the
slow-consumer policy, `Remove`-discards, `Drain`, and destructor contracts
all byte-identical, now on zero registry threads. The chain drives
`EventStreamHandle::SendAsync`, whose revocation pin spans issue to
completion: revoking (stream death) nulls the socket, closes it — failing
the in-flight operation, whose completion releases the pin — then drains
pins as before, so the ADR-0017 safety story extends to async operations
without new failure modes. An entry whose socket reports
`SupportsAsync() == false` falls back to a writer thread, so a registry
over custom blocking-only sockets keeps working with yesterday's cost
rather than failing.

**The consumer reference stays an example.** The async chat hub
(`examples/chat/async_hub_*`) mounts `on_websocket_session`, runs one
`Detached` session loop per connection over `AsyncEventStream`, fans out
through an `async_delivery` registry — many sessions, a fixed handful of io
threads — and is driven by the same generated `ChatClient`/`hub_client`
wire, in memory and as shell-driven real processes. Route matching and
input parsing are hand-written in the example: the generated streaming
serve path is deliberately not forked here (Non-goals).

## Non-goals

Generated async surfaces — coroutine handler signatures and a `co_await`
client — are the follow-up PLAN already gates on a design doc; this ADR is
that design doc's substrate, and the blocking generated API remains the
stable pre-1.0 surface (ADR-0014's ordering: the adapter wraps the blocking
pair, never the reverse). No general executor/task framework, no
third-party senders/receivers dependency, no `std::future` streaming
variant (PLAN reserves futures for the unary client).

## Alternatives rejected

- **A pending-write queue inside the session** (any number of overlapping
  `SendAsync`es). More surgery on the close-escalation logic than the
  consumers need: the registry — the fan-out driver — sends one event at a
  time by construction, and one-outstanding-with-refusal is inspectable.
  Additive later if a consumer materializes.
- **Default async implementations that secretly spawn threads** (running
  the blocking call detached). Turns the capability question into a silent
  regression — a "non-blocking" broadcast that block-parks hidden threads.
  Explicit `SupportsAsync` plus per-entry fallback keeps the cost visible.
- **Coroutines wrapping the blocking calls directly** (no transport
  primitives). Frees no threads — something still blocks per session — and
  bakes that lie into the API precisely where consumers would rely on it.
- **Generated async handlers in this slice.** Java generator + goldens +
  every fixture; PLAN §future-work requires the design doc first. The
  runtime substrate ships and is consumable today by hand-written mounts.
- **Replacing the borrowed seam.** `on_websocket` stays: the generated
  sync path and every existing consumer keep their contract; the shared
  seam is a sibling, not a successor.

## Consequences

- `WebSocket` implementors that override nothing keep exactly their old
  behavior; the async methods refuse politely. The interface grows its
  first virtuals with default bodies — implementors overriding them accept
  the one-outstanding and completion-context contracts.
- Sessions served through `on_websocket_session` do not occupy handler
  threads, so `handler_threads` returns to sizing unary work; stream
  concurrency is bounded by `max_connections` and memory, not the pool.
- Completion-context code (coroutines after `co_await`, registry chain
  steps) runs on io threads: blocking there stalls the wire for every
  session on that thread. The guides say so, loudly.
- An `async_delivery` registry holds no threads to join; its teardown is
  close-and-let-completions-drain, and TSan runs the whole matrix in CI.
- The ADR-0017 revocation invariant — no handle operation can touch a dead
  socket — now covers operations that outlive their issuing call, at the
  cost of pins that live until a completion fires; `Close` remains the
  universal unblocker.
- That unblocker has one limit, and it constrains the transports: `Close`
  releases a pin by firing the *parked* completion that holds it, so a
  terminal transition which has already taken both parked waiters must fire
  the **send** before the **receive** (#173). A receive completion can
  resume a session loop, end it, and run the pin drain inline on the
  completing thread; the send completion it would then be waiting on is a
  local in the frame below, unreachable by `Close` and by every other io
  thread. Sends release and return, so they are always safe first.
