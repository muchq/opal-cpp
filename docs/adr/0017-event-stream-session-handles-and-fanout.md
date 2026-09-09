# ADR-0017: Event-stream session handles and the fan-out registry

**Status:** Accepted (2026-07-20). Issue #112.
Implemented: `opal::eventstream::EventStreamHandle<Tx>` +
`EventStream::Share()`, `opal::server::SessionRegistry<Tx, Id>`, the
`examples/chat` hub (handler, server and CLI binaries, in-memory and
shell-driven e2e suites).

## Context

ADR-0016's handler contract lends the stream — `EventStream<Out, In>&
stream` is valid only until the method returns — and names the borrow the
async adapter's pressure point, noting the underlying sessions are already
`shared_ptr`-owned so a shared lift would be additive. Consumer feedback
(issue #112, from rebuilding a Go-style multi-client WebSocket hub on the
Phase 8 stack) confirmed the pressure arrives well before any async work:
every "N sessions, push to all" consumer must build (1) a registry of
borrowed references with remove-before-every-return discipline, where each
entry dangles the moment a handler unwinds; (2) a broadcast loop over
blocking `Send`s, which stalls a whole room behind the slowest client's TCP
window unless per-client bounded queues are rebuilt from scratch; and
(3) drain wiring, since `Stop()` aborts live sessions (ADR-0015). The
runtime already guarantees the primitives (serialized `Send`, any-thread
idempotent `Close`, FIFO order); this ADR makes the pattern safe and
provides it once.

## Decision

**A revocable owning handle, minted by the stream.** `stream.Share()`
returns `EventStreamHandle<Tx>` by value — a cheap-copy token (one shared
pointer plus an encoder) whose copies are how a session fans out: `Send`
and `Close` from any thread, no `Receive` (the one-receiver rule stays
with the stream's owner). All of a stream's handles and their copies share
one revocable view of the socket: handle operations pin it around each
socket call; when the stream object dies, its destructor closes the
session (unblocking any operation mid-call), waits for pinned operations
to drain, then revokes the pointer. After that a handle fails softly —
`Send` reports `Error::Transport`, exactly what a closed stream reports;
`Close` is a no-op. The handle therefore **never extends the session** and
adds no new failure modes; it only makes holding one safe — and because a
handle is a value that always references a view, there is no null handle
for an API to defend against. This works identically on the borrowed
(server) and owned (client) construction paths, costs nothing until the
first `Share()` (state is allocated lazily), and leaves a never-shared
stream's teardown byte-for-byte what it was. `EventStream` becomes
move-only: two owners would each claim the teardown, and handles are how a
session fans out.

**A fan-out registry over handles.** `opal::server::SessionRegistry<Tx,
Id = std::string>` maps ids to handles with a bounded outbound queue and
one writer thread per session (the Go hub's per-client goroutine, typed):
`SendTo`/`Broadcast` enqueue and return — never blocking on any wire — and
writers deliver FIFO. A full queue drops the event and runs the
slow-consumer policy: close the session by default (the defensible Go-hub
answer; the handler observes the close and unwinds), or
`Options::on_slow_consumer` to keep policy with the application. Broadcast
takes a per-recipient constructor (`Broadcast(ids, make)`) because
broadcast-identical-bytes is the wrong primitive for per-viewer state;
identical-bytes overloads exist as conveniences. `Remove` is bookkeeping,
not a close — it discards undelivered events and stops the writer, but the
session belongs to its handler; a forgotten `Remove` is a soft bug (stale
handles fail softly, delivery marks the entry broken), never a dangle. A
failed delivery is terminal for the entry; the handler observes the same
failure on its own stream and cleans up. The destructor closes every
remaining session and joins every writer, so the registry cannot
outlive-crash its threads.

**Drain stays application-side, one line.** `Stop()` keeps ADR-0015's
abort semantics; `registry.Drain(grace)` is the graceful step — `CloseAll`
(idempotent, non-blocking), then wait until the handlers' `Remove`s empty
the map, then `Stop()` finds nothing to abort. The server guide's hand-rolled
drain recipe is replaced by this.

**The consumer reference is an example, not more runtime.** Rooms,
membership, identity, and redaction policy stay application code (the
issue's non-goals): `examples/chat/hub_handler.h` shows the whole pattern —
one registry serving both converse and watch sessions (both transmit
`RoomEvents`, so `Share()` yields one handle type), ids as `room/name`,
per-viewer redaction in the broadcast callback, `Add`'s atomicity doubling
as the nickname reservation, SIGTERM → `Drain()` → `Stop()` in
`hub_server_main.cc` — exercised in memory (`hub_e2e_test.cc`) and as real
processes over real WebSockets driven by shell commands through a CLI
client (`hub_cli_test.sh`, `hub_client_main.cc`, which also demos the
handle client-side: its stdin thread sends through `Share()` while main
runs the receive loop).

## Alternatives rejected

- **`Share()` returning `std::shared_ptr<EventStreamHandle<Tx>>`** (the
  issue's sketch, and this ADR's first shipped shape). The handle is
  already internally a shared reference, so the wrapper double-layered
  ownership: an extra allocation per share, a `->` at every call site, and
  a null state every consumer had to defend (the registry grew a
  `handle == nullptr` branch for it). The value handle keeps the exact
  semantics — copies share the one revocable view — with none of that.
- **Promote `on_websocket` to hand out `shared_ptr<WebSocket>`.** True
  shared ownership of the session would let handles extend it past the
  handler — but it reworks the ADR-0015 transport seam and the generated
  serve path, splits the session's lifetime authority between transport and
  application, and buys nothing the hub needs: what consumers asked for is
  safety after the handler ends, not sessions outliving their handlers. The
  revocable view delivers that additively; the transport seam stays put for
  the async adapter to revisit.
- **Unbounded (or blocking) registry queues.** Unbounded queues turn a slow
  client into unbounded memory; blocking enqueue re-creates the stalled
  room. Bounded-drop-with-policy is the only shape that keeps `Broadcast`
  non-blocking and the failure mode explicit.
- **One dispatcher thread servicing all queues.** One blocked `Send`
  (backpressure is real, per ADR-0015) stalls every session behind it;
  per-session writers are the point.
- **Auto-`Remove` on delivery failure.** Tempting, but it makes the
  registry mutate membership behind the application's back and races the
  handler's own exit path; marking the entry broken and leaving membership
  to the owner keeps one authority.
- **`Stop(grace)` on the transport.** The transport would need to know
  about sessions it deliberately does not track (ADR-0015); with `Drain`
  a one-liner on the registry, the layering stays clean.

## Consequences

- `EventStream` is move-only (copy was never meaningful; nothing in-tree
  copied one). A stream that never calls `Share()` behaves exactly as
  before, destructor included.
- Once `Share()` has been called, destroying the stream closes the session
  as part of revocation — on the server path this is the moment the
  generated caller closed it anyway; on the client path it is the
  documented cost of sharing.
- `//runtime:server` now exports `session_registry.h` and (transitively via
  `:http`) the handle; dep-light consumers pay nothing new.
- The registry spends one writer thread per registered session — the same
  budget as the thread-per-live-stream serving model it rides on, and the
  async adapter's arrival lowers both together. The handle's shared view is
  exactly the seam that adapter needs (shared ownership of the session
  internals), so this work compounds toward it rather than away.
