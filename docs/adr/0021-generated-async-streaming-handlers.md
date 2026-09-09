# ADR-0021: Generated async streaming handlers — the coroutine serve path

**Status:** Accepted (2026-07-20). This is the follow-up ADR-0019 gated
on a design doc ("generated async surfaces — coroutine handler
signatures"), requested as the consumer assessment's named follow-on: a fully-generated
handler still pins a thread per stream, and the zero-thread mode has been
reachable only by hand-mounting a session loop via `AddSession`.
Implemented: `opal::eventstream::StreamTask`, the generated
`<Service>AsyncHandler` / `<Op>AsyncServerStream` surface, the
`<Service>Server` async constructor wiring `AddSession` routes, the
`examples/chat` async hub ported onto it, and an out-of-tree consumer
hub with shell-driven tests. Amended by ADR-0023: jsonRpc2 streams ride
the same seam through a shared-endpoint session driver
(`ServeJsonRpcSession`) plus `opal::eventstream::ReceiveMessage`, the
single-shot receive twin of this ADR's `SendMessageAwaitable`.

## Context

ADR-0019 built the async substrate — completion-driven socket primitives,
`AsyncEventStream`/`Detached`, the shared-session seam
(`on_websocket_session`, `WebSocketRouter::AddSession`/`ServeSession`),
and the registry's `async_delivery` — but deliberately stopped short of
the generator: the generated streaming serve path stayed blocking
(thread-per-stream), and the thread-free chat hub hand-mounts its route,
hand-parses its input, and hand-rolls its typed refusal, its own header
saying it "is what a hand-written async mount looks like until the
generated one lands."

That gap has a shape. Everything the hand-written mount does is code the
generator already emits for the blocking path: route registration on the
same `WebSocketRouter`, initial-request parsing (`Parse<Op>Input`),
validation refusals, event codecs (`Encode<Op>Event`/`Decode<Op>Event`),
and typed terminal exceptions (`Build<Op>ExceptionMessage`). The one
genuinely missing piece is a coroutine shape for "the handler's work is
done, possibly with a typed error": `Detached` is fire-and-forget by
design, so a `Detached`-returning handler cannot hand the framework an
`Outcome` to frame — and the blocking contract's best convenience
(return `Error::Modeled`, the server sends the exception frame and
closes) would be silently lost on the async surface.

Two lifetime facts constrain the signature. The launch callback's
`HttpRequest` and `RequestContext` (which holds a `const HttpRequest*`)
are valid only until the callback returns, so a coroutine that outlives
the launch may borrow neither; everything modeled from the upgrade
request is already in the parsed input, which is the context that
survives. And `AsyncEventStream` is move-only over an owned socket, so
something whose frame outlives the handler must own it.

## Decision

**One new runtime coroutine type, `opal::eventstream::StreamTask`.**
A lazy task whose `co_return` value is `Outcome<Unit>`: started by
`co_await`, resuming its awaiter by symmetric transfer at completion,
awaitable exactly once. A handler coroutine that throws completes with
`Error::Unknown` carrying the exception's message instead of
terminating. This is the minimal shape that restores the blocking
contract's error convenience — not a general task framework (still a
non-goal, unchanged from ADR-0019): no executors, no generic `Task<T>`,
no continuation-stealing scheduler. `Detached` remains the launch-point
type; `StreamTask` exists so generated code can *await the handler and
then act on its outcome*.

**Generated per-service: an async handler base beside the blocking one.**
For a service with streaming operations the generator now also emits:

- `using <Op>AsyncServerStream =
  opal::eventstream::AsyncEventStream<TxUnion, RxUnion>;` beside the
  blocking alias, same direction convention (Tx = server sends).
- `class <Service>AsyncHandler` — streaming operations as

  ```cpp
  virtual opal::eventstream::StreamTask
  Converse(ConverseInput input, ConverseAsyncServerStream& stream) = 0;
  ```

  Input arrives **by value** (the coroutine frame owns its copy; nothing
  borrowed from the dead upgrade request). The stream arrives **by
  reference**: the generated wrapper's frame owns it and awaits the
  handler, so the reference outlives the handler task by construction.
  There is no `RequestContext` parameter — its request pointer cannot
  legally survive the launch, and everything modeled is in the input.
  Unary operations keep their blocking signatures verbatim (a unary call
  is request/response on the handler pool either way), so one handler
  object serves the whole service.
- A second `<Service>Server` constructor taking
  `std::shared_ptr<<Service>AsyncHandler>`. Unary routes are wired
  identically; each streaming route is registered with
  `stream_router_->AddSession(...)` whose launch callback parses and
  validates the initial request exactly like the blocking route (typed
  exception + close on refusal, before any coroutine exists), then
  launches a generated `Detached` wrapper:

  ```cpp
  // generated, anonymous namespace
  opal::eventstream::Detached ServeConverseAsync(
      std::shared_ptr<ChatAsyncHandler> handler, ConverseInput input,
      std::shared_ptr<opal::http::WebSocket> socket) {
    ConverseAsyncServerStream stream(socket, EncodeConverseEvent,
                                     DecodeConverseEvent);
    auto outcome = co_await handler->Converse(std::move(input), stream);
    if (!outcome.ok()) {
      // Awaited, and best-effort: the wait keeps this frame — and the
      // stream it owns — alive until the wire has taken the refusal.
      (void)co_await opal::eventstream::SendMessage(
          socket, BuildConverseExceptionMessage(outcome.error()));
    }
    stream.Close();
  }
  ```

  The wrapper is the whole asymmetry between the seams: same parse, same
  refusals, same exception framing, same close — with the handler's
  blocking wait replaced by `co_await`, and the terminal exception itself
  *awaited* rather than fired-and-forgotten: destroying the stream closes
  the session, and a close over a busy wire may cancel the in-flight
  write (the Beast escalation), so the wrapper's frame must outlive the
  send. A blocking `Send` here would deadlock a single-io-thread
  transport; `SendMessage` is the awaitable raw-frame send that exists
  for exactly this line.

**One server instance serves one seam.** The constructor chosen decides
whether `stream_router_` carries `Add` or `AddSession` routes; the
router's existing refuse-to-mix rule and the transport's one-callback
rule then hold end to end. The header doc on `StreamRouter()` shows both
two-line mounts, keyed to the constructor used. The blocking constructor
and its emitted routes are byte-identical; the only pre-existing
emission that changes is the StreamRouter() mount doc. The async surface
is additive, and the blocking API remains the
stable pre-1.0 surface (ADR-0014's ordering, unchanged).

**Both streaming protocols get the same treatment.** The REST-JSON and
rpcv2Cbor stream-route writers grow `AddSession` siblings sharing the
launch-wrapper tail; jsonRpc2 continues to refuse event streams
entirely. Generated BUILD deps are unchanged: `AsyncEventStream`,
`Detached`, and `StreamTask` are header-only in `//runtime:http`, and
the session seam lives in `:server` — the generated server stays Beast-free.

**The chat hub moves onto the generated surface.** The hand-written
async hub main — route parsing, envelope codecs, refusal framing — is
replaced by a `ChatAsyncHandler` implementation; its shell suite
(fan-out, redaction, typed refusal, reconnect grace, drain) passes
unchanged, which is the parity proof. The hand-mount pattern remains
documented (ADR-0019, server guide) for transports and shapes the
generator does not cover. An out-of-tree consumer hub on the generated
async surface, driven by shell-commanded real processes, pins the same
loop through the module boundary.

## Non-goals

A `co_await` generated *client* (async `Exchange` on `<Service>Client`):
client-side streams pin one thread each today, which is a different,
milder cost than server thread-per-session; it waits for a consumer with
a concrete shape. A general coroutine task framework, executors, or
cancellation tokens — `StreamTask` is deliberately just "await one
handler, get its outcome." Mid-stream typed exceptions that keep the
stream open: exception frames remain terminal (ADR-0016 semantics,
both seams). Async unary handlers.

## Alternatives rejected

- **`Detached`-returning generated handlers** (the hand-written shape,
  generated). Loses the blocking contract's typed-error convenience —
  the framework can never see "done with error", so every application
  re-hand-rolls exception framing, which is exactly the code this slice
  exists to delete; the ported hub's `Kicked` refusal would be
  unwritable on the generated surface.
- **Async virtuals with default implementations on the existing
  handler.** Every blocking implementation would carry dead vtable
  slots, and "which seam is this server on" would become a runtime
  question per operation instead of a wiring-time fact per server.
- **A generated `<Service>AsyncServer` class.** Doubles the server
  surface (unary routes, `Handler()`, router accessors) to vary one
  constructor's streaming wiring; the seam is already latched per
  instance by the router.
- **Passing `RequestContext` (or the request) into the handler
  coroutine.** A use-after-free factory — the pointer dies with the
  launch callback. A sanitized copy type could exist, but everything it
  would carry that is modeled already rides the typed input; a concrete
  consumer need can add it later, additively.
- **Blocking `Send` for the terminal exception frame in the wrapper.**
  Deadlocks a single-io-thread transport when the handler completes on a
  completion context; `SendAsync` + close-in-callback is the
  contract-safe form.

## Consequences

A streaming service's zero-thread story becomes: implement
`<Service>AsyncHandler`, construct `<Service>Server` with it, mount the
two session lines — no hand-written route matching, input parsing,
envelope codecs, or refusal framing anywhere in application code. The
runtime gains one small, single-purpose coroutine type whose semantics
(lazy start, one await, throw→`Error::Unknown`) are pinned by unit
tests. The generator's streaming emission gains a second, structurally
parallel route writer; goldens grow the async surface for every
streaming fixture. The chat hub sheds its hand-written mount and becomes
the reference consumer of the generated path, and the consumer workspace
proves the same loop out of tree.
