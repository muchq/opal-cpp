# ADR-0005: Phase 1 transport and serde implementation choices

**Status:** Accepted (2026-07-06). Amended by ADR-0006: the built-in socket transport is now a
test/reference transport only; Boost.Beast (BCR modular Boost) is the supported server
transport for generated services.

## Context

PLAN Phase 1 called for libcurl (client) and Boost.Beast (server) transport
implementations, and left the serde backend open (PLAN §9, open questions 1
and 4). Both curl and Boost are heavyweight dependencies to drag through a
Bazel build matrix (Linux/macOS/Windows) for what Phase 1 actually needs:
runnable transports for the client↔server test harness.

## Decision

1. **Built-in minimal socket transport instead of curl/Beast for now.**
   `opal/http` ships `SocketHttpClient` and `SocketHttpServer`: a
   dependency-free, blocking HTTP/1.1 implementation over POSIX/winsock
   sockets (connection-per-request, `Connection: close`, 127.0.0.1 server
   binding). This fully answers PLAN open question 4's motivation — no Boost
   dependency — and is enough for integration testing and simple deployments.
   Production-grade transports (pooling, TLS, HTTP/2) arrive later as
   adapters behind the same `HttpClient`/`HttpServerTransport` interfaces
   (Phase 7); nothing in the generated code will depend on which transport is
   installed.
2. **JSON backend: nlohmann/json (BCR module), wrapped.** Only
   `opal/json`'s implementation file includes it; the public API speaks
   `Document`. Benchmarking simdjson or others is a Phase 7 activity per
   PLAN §9.
3. **CBOR codec: hand-rolled** (~450 lines including a tolerant decoder).
   nlohmann's CBOR support cannot emit the tag-1 timestamps rpcv2Cbor
   requires, and the codec is small enough that owning it — with RFC 8949
   test vectors and, in Phase 7, a fuzzer — beats patching around a library.
4. **Serde pivot: `Document`.** Generated (and hand-written prototype) serde
   converts typed structs to/from `Document`; `opal/json` and `opal/cbor`
   render `Document` to bytes. `Document` carries blob and timestamp nodes so
   each codec can apply its wire rules (base64 vs byte string; RFC 3339
   string vs tag 1). This is the simplest thing that keeps the two protocols
   symmetric. If Phase 3 protocol-test performance or fidelity demands
   direct-to-writer serde generation, that migration happens behind the
   generator, not in user code.

## Consequences

- The runtime has exactly one external C++ dependency (nlohmann/json), and
  none on the server side.
- `https://` endpoints are rejected by `ParseEndpoint` until a TLS-capable
  transport adapter exists; the error message says so explicitly.
- The built-in server is intentionally single-connection-at-a-time; PLAN
  Phase 7 (thread pools, graceful drain, limits) upgrades it or swaps it out.
- PLAN §9 open question 4 is resolved: no Boost dependency; open question 1
  is half-resolved (nlohmann now, benchmark later).
