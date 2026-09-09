# Codegen architecture

The generator (`codegen/smithy-cpp-codegen`, Java 17) is a smithy-build plugin named
`cpp-codegen`, built on `smithy-codegen-core`'s DirectedCodegen framework (ADR-0001), mirroring
smithy-rs's `codegen-core` structure (PLAN §3.2a).

## Pieces

| Class | Role |
|---|---|
| `CppCodegenPlugin` | `SmithyBuildPlugin` entry point; validates settings, rejects (for now) recursive shapes with a clear error, drives `CodegenDirector` |
| `CppSettings` | Plugin settings: `service`, C++ `namespace`, `runtimeTarget` (Bazel label of `//runtime:core` / `@smithy_cpp//runtime:core`) |
| `CppSymbolProvider` | Shape → C++ type mapping (docs/generated-types.md). A `Symbol`'s name is the full C++ type text; required `#include`s ride along in a symbol property |
| `CppWriter` | `SymbolWriter` per generated file: collects includes while the body is written, renders header comment + `#pragma once` + sorted includes + namespace wrapper. Byte deterministic |
| `DirectedCppCodegen` | Implements `DirectedCodegen`; handles structure/error/union/enum/intEnum directives, then (service directive) resolves the protocol and drives serde → client → BUILD generation |
| `TypeGenerators` | The actual C++ emission for data shapes |
| `SerdeGenerator` / `SerdeCodeGen` | `serde.h`/`src/serde.cc`: per-shape `Serialize*`/`Deserialize*` functions over the `opal::Document` pivot, in topological order (`TopologicalIndex`) |
| `ProtocolGenerator` (interface) | Per-protocol request building + response/error handling emission; `resolveProtocol` picks the implementation from the service's protocol traits |
| `HttpJsonBindingProtocol` | `alloy#simpleRestJson` (via the `simpleRestJson()` factory): HTTP bindings via `HttpBindingIndex` (labels incl. greedy, query, headers, status), JSON bodies, neutral `X-Error-Type` error identity. A thin `ProtocolGenerator` facade over `HttpJsonClientGenerator` (operation-method bodies), `HttpJsonServerGenerator` (parse/serialize functions, routes, content negotiation), and `HttpBindingCodeGen` (the header/payload/prefix-header read↔write emitters both halves share) |
| `Rpcv2CborProtocol` | `smithy.protocols#rpcv2Cbor`: fixed `/service/{S}/operation/{O}` target, `smithy-protocol` header, CBOR bodies |
| `JsonRpc2Protocol` | `smithy.cpp.protocols#jsonRpc2`: single `POST /` endpoint, JSON-RPC 2.0 envelopes, body-`method` dispatch (overrides `writeServerRoutes`), JSON-RPC error objects; streams (ADR-0023) delegate to `JsonRpc2StreamCodeGen` — opening/terminal envelopes, the shared-endpoint stream drivers on both seams (overrides `writeStreamServerRoutes`/`writeStreamSessionRoutes`) |
| `ProtocolSupport` | Shared protocol emission: error deserialization + code sanitization, `@idempotencyToken` auto-fill, header/query to-string conversion |
| `ClientGenerator` | `client.h`/`src/client.cc`: `<Service>Client` with `Create(ClientConfig)` and one method per operation |
| `BuildFileGenerator` | The generated module's `BUILD.bazel` (buildifier-clean, sorted deps) |
| `ServerGenerator` | `server.h`/`src/server.cc`: `<Service>Handler` interface + `<Service>Server` over the runtime router (routing, request parsing, response serialization, error mapping) |
| `SmokeTestGenerator` | `tests/smoke_test.cc`: generated client ↔ generated server over loopback, every operation + error mapping (user-facing, passes out of the box) |
| `ValidationGenerator` | Server-side constraint validation: per-shape `Validate*` functions (`@required`, `@length`, `@range`, `@pattern`, `@uniqueItems`, enum membership) producing the suite-exact 400 `ValidationException` failures before the handler runs |
| `IntegrationTestGenerator` | `tests/integration_test.cc`: generated client vs generated server over loopback AND real sockets — seeded random round-trips, maximal rows, per-error mapping, unknown-member tolerance (docs/design/integration-testing.md) |
| `RandomValueGenerator` | Constraint-valid, wire-exact `Random<Shape>(Rng&)` builders backing the integration suites |
| `ProtocolTestGenerator` | GoogleTest conformance suites from `smithy.test#httpRequestTests`/`#httpResponseTests`/`#httpMalformedRequestTests` (client and server cases, incl. error shapes), with the must-shrink exclusion list in `resources/.../protocol-test-exclusions.txt` |
| `TestsBuildFileGenerator` | The module's `tests/BUILD.bazel` (smoke test + conformance suites when present) |
| `NodeLiteralGenerator` / `CppLiterals` | Protocol-test `params` nodes → C++ literals constructing generated types |
| `CppCodegenRunner` | CLI main used by the `generateFixtures` Gradle task (generation without smithy-build) |

Shapes are generated in topological order (a DirectedCodegen guarantee), so the single
`include/<ns>/types.h` needs no forward declarations. Recursive shapes are rejected at the door
until boxed-recursion support lands (tracked in PLAN; the protocol-test suites prune their
recursive-shape operations for the same reason).

## Golden strategy: checked-in generated fixtures

Instead of separate golden files, the generated output for the fixture models is **checked into
the repo** and serves three purposes at once:

1. **Golden test** — CI regenerates (`gradle generateFixtures`) and fails on any byte diff, so
   every generator change shows its blast radius in review.
2. **Compile test** — the generated `BUILD.bazel`/`types.h` build as ordinary Bazel targets in
   `bazel test //...` on every platform, warning-clean at `-Wall -Wextra` (the generated BUILD
   files' own `copts`). CI enforces it: `--config=werror` (implied by `--config=ci`, see
   `.bazelrc`) promotes warnings to errors for every first-party target — generated modules
   included — on the full matrix (issue #65).
3. **Behavior test** — hand-written GoogleTests pin the generated API's semantics:
   `examples/*/generated_types_test.cc` (enums, unions, equality, optionality),
   `examples/cafe/generated_client_test.cc` (wire-level rpcv2Cbor request/response shape against
   a capturing mock transport), and `examples/weather/generated_client_e2e_test.cc` (generated
   restJson1 client against the Phase 1 hand-written server over loopback and real sockets).

This is smithy-rs's `codegen-*-test` generate→compile→run pipeline with the golden check folded
in; Phases 3–5 extend it to clients, servers, and the integration harness.

## Official protocol conformance suites

`gradle generateProtocolTests` regenerates
`protocol-tests/{simplerestjson,rpcv2cbor,jsonrpc2}/generated` from the published
`alloy-protocol-tests` / `smithy-protocol-tests` models (jsonRpc2's suite is authored in-repo
under `protocol-tests/jsonrpc2/model` — no external suite exists): a normal generated module
(types/serde/client/server) plus GoogleTest suites derived from the
`httpRequestTests`/`httpResponseTests` traits: `tests/{request,response}_tests.cc` run the
generated client against the wire (client cases), and
`tests/server_{request,response}_tests.cc` feed wire requests into the generated server and
check parsed inputs / serialized responses (server cases). The `restjson1-validation` module
generates `tests/server_malformed_tests.cc` from `httpMalformedRequestTests` (behind the
`malformedTests` plugin setting / `--malformed-tests` runner flag, on for all three modules):
malformed wire requests must be rejected with the exact error response — constraint
`ValidationException`s and parser strictness — before the handler runs. ~1,175 cases green in
CI. Two must-shrink escape hatches, both with reasons in-repo:

- **Pruned operations** (`build.gradle.kts` task args): operations using features the generator
  does not implement yet (recursive shapes, `@streaming` payloads) are removed from the model
  before generation.
- **Excluded cases** (`protocol-test-exclusions.txt`): individually skipped tests
  (`@default` population, NaN output equality, quoted-string list headers, …); stale entries
  fail generation, and the skipped ids are echoed into the generated file header.

## Regenerating

```sh
cd codegen && gradle generateFixtures        # rewrites examples/{weather,cafe}/generated
cd codegen && gradle generateProtocolTests   # rewrites protocol-tests/*/generated
```

Determinism is a hard requirement (sorted includes, model-order members, stable map iteration);
`CppCodegenPluginTest.outputIsByteDeterministic` enforces it.
