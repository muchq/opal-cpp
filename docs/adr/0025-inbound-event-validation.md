# ADR-0025: Inbound events are validated, and a violation refuses one event

**Status:** Accepted (2026-09-24). Issue #228, raised by games_hub voice
signaling (muchq/MoonBase#1590).

## Context

Generated servers validate a streaming operation's opening request against
the model's constraints, exactly like a unary route. The events that arrive
afterwards were not validated: `@length`, `@pattern`, `@range` and the rest
on event members compiled but did nothing, so a handler received whatever a
client sent. games_hub checked chat length by hand, and voice signaling was
about to add more attacker-controlled strings (SDP, ICE candidates, peer ids).

ADR-0016 makes every decoder failure terminal. That is right for a message
nobody can decode, but wrong for an event that decoded but broke a
constraint: one bad command should not end a session holding game state.

## Decision

**Generated server decoders check each decoded event.** When an inbound
event union reaches constraints, `Decode<Op>Event` runs its generated
`Validate<Union>` after deserializing. A violation returns
`Error::Validation` carrying the first failure's conformance-suite message
(for example `Value with length 281 at '/message/text' failed to satisfy
constraint: ...`). Both wires share the decoder, so the event-stream
bindings and jsonRpc2 streams get the check alike. Clients do not validate
what servers send, as with unary responses.

**`Error::Validation` from a decoder spares the session.** `EventStream`
and `AsyncEventStream` still close on any other decoder failure (ADR-0016).
A validation failure is instead returned from `Receive` with the session
left open, the way a timed receive's `TimeoutError` is.
`eventstream::SparesSession` states the rule once. No decoder returned
`Error::Validation` before this change, so the kind is unambiguous.

**The handler maps the refusal.** It sees the error from `Receive` and
decides what happens next: answer with the service's own rejection event
(games_hub's `commandRejected`), skip the event, or return and end the
session. Nothing new goes into the model: no trait names a rejection member
and the runtime adds no error frame. The handler already holds the stream
and the error, so a mapping trait would be machinery with one caller.

## Consequences

- An existing handler whose receive loop treats every error as the end
  (`if (!event.ok()) return ...`) now ends the session on a constraint
  violation, where it used to receive the invalid event. That fails closed;
  a handler that wants to keep the session checks
  `error().kind() == ErrorKind::kValidation` and continues.
- A hand-written decoder that returns `Error::Validation` now spares the
  session too. That is the intended meaning of the kind.
- The refused event is dropped. A handler that needs to correlate its
  rejection with the command (a request id inside the event) cannot, because
  it only gets the message. If a service needs that, the answer is a
  richer error detail, not a looser check.
