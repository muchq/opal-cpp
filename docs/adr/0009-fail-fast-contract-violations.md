# ADR-0009: Contract violations fail fast with context

**Status:** Accepted (2026-07-15)

## Context

ADR-0003 keeps exceptions out of the public API: every expected failure travels as a value
(`Outcome<T>`). It deliberately said nothing about *contract violations* — dereferencing the
side an `Outcome` doesn't hold, or calling a union's `as_x()` for a member it isn't holding.
The de-facto behavior was whatever `std::variant` does: a `std::bad_variant_access` with no
context, caught (if at all) by generic containment layers far from the bug. Issue #49 flagged
the consumer experience: a crash report with no code, no message, no union or member name.

Three postures were considered: return a null/sentinel (hides bugs), debug-only assert
(release builds keep the context-free throw), and fail fast with context in all builds.

## Decision

Contract violations terminate the process with a one-line message carrying the available
context, via `opal::internal::Fatal` (`opal/core/fatal.h`):

- `Outcome`: value-access on an error dies with the error's code and message;
  `value_or_die("context")` adds caller context; `error()` on a value dies naming the misuse.
- Generated unions: wrong-case `as_x()` dies naming the union, the requested member, and the
  engaged member. Checked access (`is_x()`, `as_x_or_null()`, `visit()`) never dies.

**Scope:** `Outcome` and generated unions — the API surfaces a *caller's own code* engages
with. `Document`'s `as_*` accessors are deliberately excluded for now: they sit on the wire
path, where hostile input is an expected failure, and their throws are today contained into
protocol-level 500s (and exercised that way by the fuzz harnesses). Extending fail-fast there
would need its own decision.

There is deliberately **no fatal-handler hook**: a hook would be speculative generality at
this project's size, and a redefinable fatal path invites recovering from states the runtime
has declared unrecoverable.

## Consequences

- Crash lines carry the diagnosis (`smithy: OrderStatus::as_ready(): engaged member is
  pending`), and death tests pin the exact formats.
- **Server posture shift:** before, a handler's wrong-side deref threw and the dispatch
  guard contained it into a 500 response; now the same bug kills the process. This is
  deliberate — a contract violation is a bug, not a request-scoped failure, and containing it
  hides corruption — but operators of multi-tenant handlers should know a latent wrong-case
  access is now fatal rather than a per-request 500.
- Fuzzing is unaffected: generated serde and dispatch guard every union and `Outcome` access,
  so wire input cannot reach the new aborts; only user-authored code can.
