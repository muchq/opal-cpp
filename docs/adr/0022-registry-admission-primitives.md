# ADR-0022: Registry admission primitives — ResumeOrAdd and Close

**Status:** Accepted (2026-07-21). Issue #122, the promotion the ADR-0020
and ADR-0021 review panels twice deferred. Implemented:
`SessionRegistry::{Admission, ResumeOrAdd, Close}`, the three example
admission loops deleted, the production guide's canonical block replaced
by the one call. Amended 2026-10-13: `Close` does not bound a silent
peer, so the kick is `Kick` (below).

## Context

ADR-0020 documented reconnect admission as a recipe: Resume first, fresh
Add second, retried briefly because a reconnect can beat the old wire's
failure notice — for a beat the id looks live to both paths, and the
race is registry-inherent, not application protocol. By ADR-0021 the
recipe had three verbatim in-repo copies (the async hub and both
consumer reconnect mains) plus a canonical block in the production
guide, and the promotion trigger the panels named — the rule-of-three
met on a loop consumers will copy and mis-retype — had fired. The same
reviews named the recipe's dead end: when the refusal is wrong (a
half-dead session whose wire sent no FIN), the application had no
primitive to kick the live entry, so the retry loop only converged when
the old wire's failure was eventually observed.

## Decision

**`ResumeOrAdd(id, mint, deadline) -> Admission`.** The recipe as a
method: per attempt, `Resume` then `Add`; between attempts, a short
sleep; `kRefused` only once `deadline` is spent (a zero deadline is the
single-shot form). `mint` is a handle *factory* — each attempt needs a
fresh `Share()`. Callers get the three-way `Admission` outcome because
every consumer branches on it: `kResumed` replays the snapshot,
`kAdded` announces the join, `kRefused` sends the collision refusal.
The method **blocks**, and the contract rides the doc comment where the
panels insisted it live: call it before a handler's first suspension —
on the launching thread, where brief blocking is fine — never from a
completion context. That contract is prose because it cannot be a type;
relocating the loop does not relocate the constraint, only states it
once instead of three times.

**`Close(id) -> bool`.** The kick primitive the refusal branch was
missing: closes the id's current session — the handle copy is taken
under the entry lock (the Resume-swap discipline, ADR-0020's panel
round) and closed outside all locks. The session's handler observes the
close and runs its normal exit path, leaving the id admittable on the
next dial: freed outright after a Remove exit, parked-resumable after a
Detach exit (where the redial's ResumeOrAdd reports kResumed and the
snapshot carries the identity across).
Policy stays with the application: `ResumeOrAdd` never kicks on its
own, and the chat examples deliberately keep refusing name collisions
rather than kicking — `Close` exists so an application that *knows* the
old session is dead (a silent partition, an operator action) has a
convergent path instead of waiting out TCP. On a detached entry it
closes an already-closed handle: a harmless no-op — detached entries
are `Resume`'s and `Remove`'s business.

## Amendment (2026-10-13): Kick

`Close` was meant to be the kick for "a silent partition … instead of
waiting out TCP", and on Beast it is not one. Its close is the graceful
handshake: send the close frame, then read until the peer's close
arrives, bounded only by Beast's handshake timeout — the transport's
`request_timeout_seconds`, 30s by default. A peer that is gone never
answers, so the old handler holds the id for that long, and a
`ResumeOrAdd` behind it is refused the whole time. games_hub found it:
a phone that dropped off the network kept its player locked out of a
chess game for half a minute after it came back.

**`Kick(id, why, grace) -> bool`** is the kick. It ends the session like
`Close`, with two differences, both carried by new `WebSocket` members
(the runtime's transports — Beast, the in-memory pair, the dialed
client, the JSON-RPC wrapper — override them; the base defaults to
`Close()` and reads nothing, which is what test fakes and the consumer
example's socket get):

- **`WebSocket::Kick(CloseReason why, grace)`** puts `why` — a close code
  and reason — on the close frame and waits at most `grace` for the
  peer's answer before tearing the connection down. On Beast the kick
  arms its own deadline that closes the socket `grace` later, whatever
  the close is waiting on — the peer's answer, a keep-alive frame
  holding Beast's write lock, a Close's handshake already running, or a
  close deferred behind a wedged write. Only the first kick arms it, so a
  second neither re-arms it nor cuts the first one's frame short. A peer
  that is still there reads the code; one that is gone costs `grace`.
- **`WebSocket::PeerCloseReason()`** is the close frame the peer ended the
  session with. With an application code (4000-4999) the server can say
  *why* it dropped a client — "superseded by a newer connection" — and a
  client can stop redialing instead of taking its seat back, which is what
  keeps two connections under one identity from trading it forever.

`EventStreamHandle::Kick` is the handle's form. Codes outside 1000 and
3000-4999, or a reason over 123 bytes, fail fast (ADR-0009): the
protocol's own codes are the transport's to send.

`Close` keeps its meaning — the polite end of a session whose peer is
expected to answer — and its doc no longer claims the silent-partition
case.

A handle's operations also stopped pinning a socket the stream owns. A
transport may complete operations inline on the calling thread — the
in-memory pair does, as its contract allows — and the loop then ends and
destroys its stream inside the handle's call, where the stream's teardown
waited for the pin that call held. Any hub calling `Close(id)` on a
pair-backed session parked in `Receive` deadlocked, and so did a delivery
the peer answered by closing. The coroutine adapter owns its socket, so a
handle now holds a strong reference across each call (to completion, for
`SendAsync`) instead; a borrowed socket is still pinned, and a borrowing
stream is never torn down by its own socket's completions.

## Non-goals

An async admission shape (`co_await ResumeOrAdd`) — admission runs
pre-first-suspend by design, where blocking is legal and cheap; an
awaitable variant would invite admission from completion contexts, the
exact misuse the contract forbids. Auto-kick policies (kick-on-timeout,
takeover tokens): application protocol, per ADR-0020's posture.
Changing `Resume`/`Add` themselves: the primitives compose unchanged.

## Consequences

Consumers write one call and branch on the enum; the retry cadence and
ordering subtleties live (and are tested) in exactly one place. The
guide teaches the call instead of the loop. The silent-partition story
gains its missing move: `Kick`, since the amendment above. The registry's public surface grows two
methods and one enum, all shallow compositions of existing primitives —
no new locking, no new threads, no new states.
