# ADR-0003: Outcome-based error handling; no exceptions across the generated API

**Status:** Accepted (2026-07-06)

## Context

Generated C++ APIs need an error-reporting convention. C++ codebases are split between
exceptions and result types, and many large consumers build with `-fno-exceptions`.

## Decision

- Generated client operations and server handler methods return `opal::Outcome<T, Error>`, an
  `std::expected`-like result type (polyfilled until C++23 is table stakes; the alias switches to
  `std::expected` when the floor rises).
- Modeled errors derive from `opal::ModeledError`; transport and deserialization failures use
  distinct error categories, all carried in `Outcome::error()` with code, message, and
  retryability metadata, plus `ErrorsAs<T>()` accessors for typed access.
- Exceptions never cross the generated API boundary. The runtime must be buildable with
  `-fno-exceptions`; internal use of exception-throwing std APIs is avoided in hot paths.
- **Contract violations fail fast; recoverable bad config returns an error; neither throws.** A
  composition-time *programming* error — a null callback, a health-check name that would corrupt
  the failing-list JSON — is not a condition a caller can handle, so it aborts via
  `opal::internal::Fatal` (ADR-0009) rather than throwing `std::invalid_argument`. Bad *config*
  the caller can act on — a malformed proxy-trust CIDR (`TrustedProxies::Parse`), like a bad bind
  address in `Start` — returns an `Error::Validation` through an `Outcome`. Both replace the old
  `throw`, so both compile under `-fno-exceptions`; the split is the ADR-0009 line: fail-fast for
  "the program is wired wrong," an error `Outcome` for "the input/config is bad."
- **Exceptions are contained at every boundary that must not unwind.** Nothing in the runtime lets
  an exception cross an `Outcome`-returning entry point or escape a transport io thread. Wire-facing
  callbacks in the `-fno-exceptions`-clean runtime (request handlers, readiness probes, metrics
  sinks) run inside `opal::internal::Contain` (`opal/core/exception_guard.h`), which compiles to
  a direct call under `-fno-exceptions`. The Beast transport — which cannot build `-fno-exceptions`,
  so it always has exceptions — carries its own containment: each background `io_context::run()` is
  wrapped in a catch-and-re-enter backstop so a stray throw drains remaining work instead of
  terminating the process; its handler-pool posts and WebSocket completion callbacks are contained
  at the point of the call; and its `Send`/`Start`/`FromConfig` Outcome boundaries are guarded
  against resource-construction failures (`std::bad_alloc`, a `std::thread` ctor under load).

## Enforcement

A dedicated CI leg (`bazel build --config=noexcept`, the `noexcept` job / `make noexcept`) builds
the dependency-light runtime with `-fno-exceptions`. Boost.Asio/Beast are not `-fno-exceptions`-clean,
so `//runtime:http_beast` and its dependents are out of scope; every other runtime library is in.
A build is the gate — the object code must exist with no reachable `throw` or `catch`.

## Consequences

- Works for exception-free codebases; callers who prefer exceptions can trivially wrap.
- Server handlers report modeled errors by returning them, which keeps handler signatures
  symmetric with client operation signatures — important for the client↔server integration
  harness that reuses fixture handlers.
- C++20 baseline: gcc 11+ / clang 14+ / MSVC 19.30+ (confirmed here from PLAN §9 open question 3).
