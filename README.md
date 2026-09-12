# opal-cpp

Smithy code generators for C++ — generate idiomatic C++ clients and servers from
[Smithy](https://smithy.io) models, plus `opal`, the shared C++ runtime they build on.

**Start here → [docs/quickstart.md](docs/quickstart.md):** empty directory to a generated C++
client integration-testing a generated C++ server, in one Bazel module — no prior Smithy
experience assumed. Day 2 (evolving the model) is
[docs/model-evolution.md](docs/model-evolution.md).

- **Vendor-neutral:** implements Smithy and its protocol specs; nothing AWS-specific. The REST
  protocol is [`alloy#simpleRestJson`](https://disneystreaming.github.io/smithy4s/docs/protocols/simple-rest-json/overview/)
  (the neutral protocol smithy4s uses — so opal-cpp clients and smithy4s services interoperate).
- **Three protocols:** `alloy#simpleRestJson` (REST/JSON), `smithy.protocols#rpcv2Cbor` (RPC/CBOR),
  and `smithy.cpp.protocols#jsonRpc2` (RPC/JSON over JSON-RPC 2.0) — all vendor-neutral.
- **Bazel-native:** Bazel 9 is the sole supported build system for the repo and consumers.
- **Client tests server:** generated clients integration-test generated servers in CI.

## What works today (0.2.0)

- **Clients and servers for all three protocols**, each green against a conformance suite in CI
  (the official alloy and rpcv2Cbor suites, an authored one for jsonRpc2) with documented,
  must-shrink exclusion lists.
- **The full generated surface:** typed structs/unions/enums, serde, HTTP bindings, constraint
  validation with suite-exact `ValidationException` output (ReDoS-safe `@pattern`), typed
  modeled errors, paginators, idempotency tokens, and gzip request compression.
- **In-graph generation:** `smithy_cpp_{types,client,server}_library` run the generator
  hermetically inside the Bazel build graph — no scripts, no JVM to install. An out-of-tree
  consumer module is CI-tested on Linux/macOS (Windows was dropped in ADR-0008), plus a CLI for
  generating elsewhere.
- **Production serving and calling** over Boost.Beast with TLS in both directions, retries with
  jittered backoff, client interceptors, server middleware, and bearer/API-key auth wiring.
- **Hardening in CI:** sanitizer jobs, libFuzzer harnesses, hostile-input test banks, and every
  fixture's generated client integration-testing its generated server.

## Current limitations

Consolidated in one place — if your API depends on any of these, check here before adopting:

- **`@streaming` blobs stream on the client only.** A `@streaming` blob bound as an
  operation's *response* `@httpPayload` puts a defaulted `opal::http::BodyWriter` on the
  generated client method: the bytes go to the writer as they arrive and the member is
  left empty ([production-guide](docs/production-guide.md#through-a-generated-client)).
  Everywhere else the blob is still an ordinary `opal::Blob`, fully buffered in memory:
  request payloads (writing one needs chunked request framing), the RPC protocols (every
  member rides one document), and the whole server half. Event streams, by contrast, are
  real
  ([ADR-0016](docs/adr/0016-generated-event-streams.md)): a `@streaming` union operation
  generates a typed `opal::eventstream::EventStream` session over WebSocket for all
  three protocols — `simpleRestJson` and `rpcv2Cbor` ride the event-stream framing codec
  ([ADR-0014](docs/adr/0014-event-stream-framing-first.md)) and `jsonRpc2` streams
  JSON-RPC 2.0 envelopes natively ([ADR-0023](docs/adr/0023-jsonrpc2-event-streams.md)) —
  over the WebSocket transports
  ([ADR-0015](docs/adr/0015-websocket-transports.md)). The full-duplex chat example
  ([`examples/chat/`](examples/chat/)) runs generated client ↔ generated server over real
  WebSockets in CI, and browsers join codec-free: `simpleRestJson` over the negotiated
  JSON-text wire ([ADR-0018](docs/adr/0018-json-text-event-stream-frames.md)), `jsonRpc2`
  over plain JSON-RPC text frames. Scoping edges (`@eventHeader` / `@eventPayload`,
  initial-response members, and — outside `jsonRpc2`, whose opening call carries them —
  body-bound initial-request members) are rejected with generation-time diagnostics.
- **No Bazel Central Registry / Maven publishing** — consumers pin a git commit
  ([quickstart](docs/quickstart.md)); publishing is deferred until the project is
  production-validated (#44 tracks release readiness).
- **Linux and macOS only** ([ADR-0008](docs/adr/0008-drop-windows-support.md) dropped Windows).

Roadmap and per-phase status live in [`docs/PLAN.md`](docs/PLAN.md).

## Documentation

| Doc | What it covers |
|---|---|
| [quickstart.md](docs/quickstart.md) | Model → generated client + server, from an empty directory |
| [model-evolution.md](docs/model-evolution.md) | Day 2: changing the model, regeneration, drift detection |
| [generated-types.md](docs/generated-types.md) | The Smithy → C++ mapping contract |
| [server-guide.md](docs/server-guide.md) | What the generated server does before/after your handler |
| [production-guide.md](docs/production-guide.md) | Real transports, TLS, retries, auth, middleware |
| [runtime.md](docs/runtime.md) | The `opal-cpp-runtime` library, module by module |
| [development.md](docs/development.md) | Building, testing, and linting this repo |
| [versioning.md](docs/versioning.md) | Compatibility policy; [CHANGELOG.md](CHANGELOG.md) has releases |
| [PLAN.md](docs/PLAN.md) | The phased roadmap; [adr/](docs/adr) records architecture decisions |
| [design/](docs/design) | Internals: codegen architecture, the integration-test harness; [fuzzing.md](docs/fuzzing.md) covers the fuzz setup |

## Building

```sh
bazel test //...                     # C++ runtime (requires bazelisk)
cd codegen && gradle build           # Smithy → C++ generator (JVM)
```

Bazel runs through [bazelisk](https://github.com/bazelbuild/bazelisk), which reads
[`.bazelversion`](.bazelversion) and fetches the pinned release. The first build downloads
the toolchain and all dependencies — see the quickstart's
[first-build section](docs/quickstart.md#the-first-build-cost-caching-and-locked-down-networks)
for what to expect and for proxy/offline setups.

See [`docs/development.md`](docs/development.md) for details and
[`CONTRIBUTING.md`](CONTRIBUTING.md) for contribution guidelines.

## License

[Apache 2.0](LICENSE)
