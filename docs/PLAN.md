# smithy-cpp — Phased Implementation Plan

A plan for building **smithy-cpp**: a [Smithy](https://smithy.io) code generator that produces
idiomatic, well-tested C++ **clients** and **servers**, plus the shared C++ **runtime library**
the generated code depends on. A core requirement threaded through every phase: **generated
clients are used to integration-test generated servers**, so the two halves continuously verify
each other.

---

## 1. Goals

1. **Client generation** — from a Smithy model, generate a C++ client SDK: typed request/response
   structures, operation methods, serialization, error handling, and a pluggable HTTP transport.
2. **Server generation** — from the same model, generate a C++ server scaffold: request routing,
   deserialization, input validation, a handler interface the user implements, and response
   serialization.
3. **Round-trip verification** — every example/test service is exercised end-to-end by running the
   generated client against the generated server, in CI, on every commit.
4. **Well tested** — a layered test strategy: runtime unit tests, codegen snapshot ("golden") tests,
   compile tests of generated code, Smithy protocol compliance tests, client↔server integration
   tests, fuzzing and sanitizers.
5. **Documented** — user-facing quick start, per-feature guides, generated-code reference, and
   architecture/decision records for contributors.
6. **Easy to use** — one Bazel rule invocation (or one CLI command) to go from `model.smithy` to
   a compiled client or server target. **Bazel 9 is the sole supported consumer build system**:
   native rules (`smithy_cpp_client_library` / `smithy_cpp_server_library`), a bzlmod module, and
   Bazel Central Registry distribution. No CMake support is provided or maintained.
7. **Bidirectional streaming** — event streams (`@streaming` unions) over HTTP and WebSocket
   transports, on both client and server, is an explicit goal. It is deliberately deferred to a
   dedicated post-0.1.0 phase (Phase 8), but the transport and serde abstractions from Phase 1
   onward are designed so it slots in without breaking API changes.

### Non-goals (for the initial releases)

- AWS service SDKs / SigV4 / AWS-specific traits (the design must not preclude them, but they are
  out of scope; the AWS SDK for C++ already exists).
- Every Smithy protocol. We start with **restJson1 + rpcv2Cbor** (see §3.3) and add others later.
- HTTP/2-specific features and MQTT bindings — deferred (§9). Bidirectional streaming is *not* a
  non-goal: it is planned, as Phase 8.

---

## 2. Guiding principles

- **Thin generated code, rich runtime.** Generated code should be boring glue over a hand-written,
  well-tested runtime library. Bugs get fixed once in the runtime, not N times in templates.
- **Idiomatic C++20.** `std::optional` for optional members, `std::variant`-backed unions,
  `std::expected`-style outcomes (polyfilled until C++23 is table stakes), RAII everywhere, no
  exceptions across the generated API boundary by default (configurable).
- **Pluggable transports.** Generated code targets abstract `HttpClient` / `HttpServer` interfaces;
  concrete implementations (libcurl client, Boost.Beast/asio server) live behind them and are
  swappable — which is also what makes in-process integration testing cheap.
- **Golden tests for codegen.** Every generator feature lands with checked-in expected output; a
  diff in generated code is always a deliberate, reviewed event.
- **The client tests the server.** From the first phase where both exist, CI compiles an example
  service both ways and runs the generated client against the generated server.
- **Stand on smithy-rs's shoulders.** smithy-rs (§3.2a) already solved client+server generation
  for a systems language with the same codegen framework; we port its architecture and test
  strategy to C++ instead of rediscovering them.
- **Vendor-neutral.** smithy-cpp implements the Smithy specification and its protocol specs —
  nothing AWS-specific: no SigV4, no AWS traits, no endpoint/region logic, no AWS SDK behaviors.
  AWS-adjacent artifacts are limited to what protocol specs mandate (e.g. restJson1's
  error-discriminator header) and the reuse of official protocol-test suites and smithy-rs as
  prior art.
- **Bazel-native, Bazel-only.** Bazel 9 is the single build system, for this repo and for
  consumers. Every user-facing capability — generation rules, runtime consumption, the
  integration-test pattern — ships as ordinary Bazel targets, and generation runs hermetically
  inside the build graph. There is no second build system to keep in parity.

---

## 3. Architecture

### 3.1 Components

```
smithy-cpp/
├── codegen/                  # The generator (JVM, Smithy DirectedCodegen) — §3.2
│   ├── smithy-cpp-codegen/           # core: symbol provider, type/serde generation
│   ├── smithy-cpp-codegen-client/    # client-specific generation
│   ├── smithy-cpp-codegen-server/    # server-specific generation
│   └── smithy-cpp-codegen-tests/     # golden tests, protocol-test generation
├── runtime/                  # C++ runtime the generated code links against — Phase 1
│   ├── include/opal/core/          # types: Blob, Timestamp, Document, Outcome, Error
│   ├── include/opal/json/          # JSON serde primitives (restJson1)
│   ├── include/opal/cbor/          # CBOR serde primitives (rpcv2Cbor)
│   ├── include/opal/http/          # HttpRequest/Response, HttpClient/HttpServer interfaces
│   ├── include/opal/client/        # client base: config, endpoint, retry, interceptors
│   ├── include/opal/server/        # server base: router, handler dispatch, validation
│   └── src/…                         # implementations: curl transport, beast server, …
├── bazel/                    # Bazel rules: smithy_cpp_*_library, toolchains — Phase 6
├── MODULE.bazel              # bzlmod module root (Bazel 9, bzlmod-only)
├── examples/                 # example models + services (also used as test fixtures)
├── tests/                    # cross-cutting: integration harness, fuzzers
└── docs/                     # user guides, design docs, ADRs
```

### 3.2 Generator technology (decision)

**Decision: write the generator on the JVM (Java 17) using Smithy's official
`software.amazon.smithy:smithy-codegen-core` `DirectedCodegen` framework**, packaged as a
`smithy-build` plugin (like smithy-typescript, smithy-go, smithy-swift, smithy-rs).

Alternative considered: a standalone generator (in C++ or Python) that consumes the JSON AST
produced by `smithy build`. It keeps the toolchain single-language but forfeits the pieces the
Smithy team maintains for us — model validation, trait indexes (`HttpBindingIndex`,
`OperationIndex`), symbol/dependency management, topological shape ordering, protocol-test trait
plumbing, and forward compatibility as the IDL evolves. Every mature third-party Smithy generator
uses the JVM framework; we should too. The JVM is a **build-time-only** dependency: consumers of
generated code and the runtime never need Java at runtime, and Phase 6 wraps the JVM invocation so
users only ever type a Bazel target / CLI command. (Recorded as ADR-0001 in Phase 0.)

### 3.2a Reference implementation: smithy-rs

[smithy-rs](https://github.com/smithy-lang/smithy-rs) is the closest prior art — the only
official Smithy generator that ships **both a client and a server generator plus the runtime**,
which is exactly our shape. We will treat it as the reference implementation and consult it
before designing each subsystem, mapping its structure onto ours:

| smithy-rs (Kotlin/Rust) | smithy-cpp equivalent | Used in |
|---|---|---|
| `codegen-core` (SymbolProvider, `RustWriter`, protocol serde generators shared by client & server) | `smithy-cpp-codegen` core module, `CppWriter` | Phase 2 |
| `codegen-client` + `ClientProtocolTestGenerator` | `smithy-cpp-codegen-client` + client protocol-test generation | Phase 3 |
| `codegen-server` (routing, constraint traits, `ValidationException`, `ServerProtocolTestGenerator`) | `smithy-cpp-codegen-server` | Phase 4 |
| `codegen-client-test` / `codegen-server-test` (Gradle projects that generate code from a corpus of test models, then compile and `cargo test` it) | our generate→compile→run test harness over the fixture corpus | Phases 2–5 |
| `rust-runtime/` crates: `aws-smithy-types`, `aws-smithy-http`, `aws-smithy-runtime`, `aws-smithy-http-server` | `runtime/` modules: `opal/core`, `opal/http`, `opal/client`, `opal/server` | Phase 1 |

Concretely: its shared-core/client/server module split, its "generate real projects from test
models and run their tests in CI" strategy, its server constraint-validation design
(`ConstraintViolation` → structured `ValidationException`, RFC'd in smithy-rs), and its
protocol-test generators are all patterns we adopt rather than reinvent. Where we deliberately
diverge (e.g., golden-file tests in addition to compile-and-run tests, C++ error-handling
idioms instead of Rust `Result`), the divergence is recorded in the relevant design doc.

### 3.3 Initial protocols (decision)

**Ship both a REST and an RPC protocol from the start: `restJson1` and
`smithy.protocols#rpcv2Cbor`.** `restJson1` (HTTP bindings + JSON bodies) exercises the full
HTTP binding surface (labels, query params, headers, payloads, status codes) that a server
router needs; `rpcv2Cbor` — the vendor-neutral RPC protocol defined in the core `smithy.protocols`
namespace — exercises the pure-RPC path (`POST /service/{Service}/operation/{Operation}`, CBOR
bodies). Both have official protocol-test suites (restJson1 in `smithy-aws-protocol-tests`,
rpcv2Cbor in `smithy-protocol-tests`). Implementing two structurally different protocols —
different bindings *and* different wire formats (JSON vs CBOR) — **in the same phases** forces
the protocol-generator and serde abstractions to be real from day one instead of retrofitted.
Further protocols slot in behind the same interface later (§9).

A note on vendor neutrality: `restJson1` lives in the `aws.protocols` trait namespace for
historical reasons, but it is the de-facto standard protocol for generic (non-AWS) Smithy REST
services — smithy-rs's generic server targets it — and carries no AWS coupling beyond
protocol-mandated names (e.g. its error-discriminator header). smithy-cpp implements protocol
specs and nothing else: no AWS traits, endpoints, auth, or SDK behaviors (see §2).

> **Superseded before 0.1.0 (Phase 7e).** We used `restJson1` through Phases 3–7 precisely
> because its official conformance suite (~526 cases) is the best possible hardening for the
> HTTP-binding + JSON-serde stack. But every Smithy REST/JSON protocol is AWS-namespaced, so
> shipping `restJson1` as our headline REST protocol contradicts the vendor-neutrality goal.
> Phase 7e replaces it with a neutral `smithy.cpp.protocols#httpJson` (see below). The only
> vendor-neutral protocol Smithy itself defines is `smithy.protocols#rpcv2Cbor`, which we keep.

### 3.4 Generated API shape (sketch, refined in Phase 2/3/4 design docs)

```cpp
// Client
opal::ClientConfig cfg;
cfg.endpoint = "http://localhost:8080";
WeatherClient client{cfg};
auto outcome = client.GetForecast(GetForecastInput{.city = "Seattle"});
if (outcome) { use(outcome->chanceOfRain); } else { log(outcome.error().message()); }

// Server — user implements a generated pure-virtual handler interface
struct MyWeatherService final : WeatherServiceHandler {
  GetForecastOutcome GetForecast(const GetForecastInput& in, const RequestContext& ctx) override;
};
opal::ServerConfig scfg{.port = 8080};
WeatherServiceServer server{std::make_shared<MyWeatherService>(), scfg};
server.serve();   // or server.start() / server.stop() for tests
```

---

## 4. Phases

Each phase lists **Goals**, **Tasks**, **Testing**, **Docs**, and **Exit criteria**. Phases are
sequential milestones, but small overlaps are expected (e.g., Phase 4 design can start while
Phase 3 stabilizes). Indicative effort assumes 1–2 engineers.

---

### Phase 0 — Foundations (≈1–2 weeks)

**Goals:** a repo where every later phase has rails: builds, CI, style, decisions recorded.

**Tasks**
- Repo layout as in §3.1; `MODULE.bazel` + `BUILD` files for the runtime, Gradle build for
  codegen.
- Toolchain baseline: C++20; **Bazel 9** (bzlmod-only — no WORKSPACE support); Java 17 + Gradle
  for codegen; GoogleTest via `@googletest` (bzlmod); nlohmann/json (or simdjson later) vendored
  behind the runtime's serde interface.
- CI (GitHub Actions):
  - Bazel build+test matrix for the runtime: Linux gcc + clang, macOS clang, Windows MSVC.
  - ASan/UBSan job on Linux clang (via `.bazelrc` `--config=asan`/`ubsan`).
  - Codegen: Gradle build + unit tests.
  - `clang-format` + `clang-tidy` + `.editorconfig` checks; Spotless for Java; `buildifier` for
    Bazel files.
- Author ADR-0001 (JVM generator, §3.2), ADR-0002 (restJson1 + rpcv2Cbor from the start, §3.3),
  ADR-0003 (error handling: `Outcome<T, Error>` return values, no exceptions across generated
  API), ADR-0004 (Bazel 9 as the only supported build system, for this repo and for consumers;
  no CMake).
- Check in the **model corpus**: `examples/weather` (simple REST CRUD) and `examples/cafe`
  (unions, enums, streaming-payload placeholder, pagination) — these fixtures drive tests in every
  later phase.
- `CONTRIBUTING.md`, issue/PR templates, license.

**Testing:** CI green on all platforms with a hello-world runtime target and empty codegen module.

**Docs:** `README.md` with project vision + status table; ADRs; `docs/development.md` (how to
build/test both halves).

**Exit criteria:** a contributor can clone, `bazel test //...`, and get green on all three OSes
with documented commands; ADRs 1–4 merged.

---

### Phase 1 — Runtime core: `smithy-cpp-runtime` (≈3–4 weeks)

**Goals:** the hand-written C++ library that generated code will call into. No codegen yet — the
runtime is designed against *hand-written* mock "generated" code for the weather example, which
later becomes the golden reference for Phase 2/3 output. Module boundaries deliberately mirror
smithy-rs's runtime crates (§3.2a): before finalizing each module's API, review the corresponding
`aws-smithy-*` crate for the problems it already solved (e.g., `aws-smithy-types` for
timestamp/document edge cases, `aws-smithy-http-server` for router and rejection design). Every
module is an ordinary `cc_library` Bazel target (ADR-0004).

**Tasks**
- **Core types** (`opal/core`):
  - `Blob`, `Timestamp` (epoch-seconds / date-time / http-date parsing+formatting), `Document`
    (JSON-like dynamic value), `BigDecimal`/`BigInteger` placeholders.
  - `Outcome<T, E>` (expected-like), `Error` hierarchy: `ModeledError` base, transport errors,
    deserialization errors; error code + message + retryability metadata.
- **Serde** behind a format-agnostic reader/writer interface (both protocols' generated serde
  code targets the same interface shapes):
  - **JSON** (`opal/json`): thin wrappers over the JSON backend with Smithy-specific behaviors
    (timestamps, blobs as base64, sparse vs dense collections, unknown member skipping for
    forward compatibility).
  - **CBOR** (`opal/cbor`): deterministic encoder + tolerant decoder per the rpcv2Cbor spec
    (definite/indefinite lengths, tagged timestamps, unknown member skipping).
- **HTTP layer** (`opal/http`):
  - Value types: `HttpRequest`, `HttpResponse`, `Headers` (case-insensitive), `Uri` with
    escaping rules matching Smithy httpLabel/httpQuery requirements.
  - Interfaces: `HttpClient` (async-capable: `send(request) -> future<Response>` plus sync
    convenience), `HttpServerTransport`. Both interfaces are designed with Phase 8 in mind:
    message bodies are stream-shaped (not forced into contiguous buffers), and the server
    transport can surface connection upgrade requests — so event streams and WebSockets later
    extend the interfaces rather than break them.
  - Implementations: **libcurl** `HttpClient`; **Boost.Beast/asio** `HttpServerTransport`; plus
    an **in-memory loopback transport** that connects an `HttpClient` directly to a server request
    handler with no sockets — the backbone of fast integration tests later.
- **Client base** (`opal/client`): `ClientConfig` (endpoint, timeouts, user-agent), interceptor
  hook points (before-send/after-receive), simple retry policy (off by default until Phase 7).
- **Server base** (`opal/server`): `Router` (method + URI-pattern matching with literal >
  label precedence per Smithy spec), `RequestContext`, error→HTTP response mapping, and a
  `ValidationFailure` type for Phase 4's constraint checks.

**Testing**
- GoogleTest unit tests per module; target ≥90% line coverage on `core`, `json`, `http` value
  types (coverage job added to CI).
- URI/label/query escaping tested against the tables in the Smithy HTTP binding spec.
- Timestamp round-trip tests across all three formats, including edge cases (fractional seconds,
  pre-epoch).
- Loopback transport tested with a hand-written echo service.
- ASan/UBSan on all runtime tests in CI.

**Docs:** Doxygen set up and published (GitHub Pages) for the runtime API; `docs/runtime.md`
overview; each interface header carries usage examples.

**Exit criteria:** a **hand-written** weather client and server built on the runtime pass an
end-to-end request over both loopback and real sockets in CI. This hand-written pair is the
prototype the generators must reproduce.

---

### Phase 2 — Codegen skeleton + type generation (≈3–4 weeks)

**Goals:** the smithy-build plugin exists; from a model it generates a compiling Bazel package
containing all **data types** — no operations yet.

**Tasks**
- `CppCodegenPlugin` implementing `SmithyBuildPlugin` (`"cpp-codegen"` in `smithy-build.json`),
  wired through `DirectedCodegen`: `CppSettings` (namespace, module name, client/server/both),
  `CppSymbolProvider`, `CppWriter` (a `SymbolWriter` with `#include` management and
  forward-declaration handling), `CppDelegator`. Before writing each piece, review the
  smithy-rs `codegen-core` counterpart (`RustSymbolProvider`, `RustWriter`, its
  `CodegenDecorator` extension mechanism) and adopt its structure where it transfers.
- **Symbol provider**: shape → C++ mapping (structure→struct, string enum→`enum class` +
  unknown-value preservation, union→`std::variant` wrapper with visitor helpers, list→`std::vector`,
  map→`std::map`/`unordered_map`, optionality via `std::optional`, recursive shapes via
  `std::unique_ptr` indirection, C++ reserved-word and keyword escaping, PascalCase/camelCase
  conventions documented and fixed here).
- Generate: headers + sources for all shapes in closure, a `BUILD.bazel` for the generated
  module (a `cc_library` depending on `smithy-cpp-runtime`), equality operators, and
  builder-style construction (designated initializers where possible).
- Deterministic output (stable ordering) — a hard requirement for golden tests.
- Stand up the **generate→compile→run test pipeline** modeled on smithy-rs's
  `codegen-client-test`/`codegen-server-test`: a Gradle task generates projects from every
  fixture model and drives their C++ builds+tests; this pipeline is what Phases 3–5 extend.

**Testing**
- **Golden tests**: for each fixture model, generated files are compared byte-for-byte against
  checked-in expected output (`codegen-tests/golden/…`); a Gradle task regenerates goldens
  deliberately.
- **Compile tests in CI**: generate from the fixture corpus, then `bazel build` the generated
  packages against the runtime — generation isn't "done" until the output compiles warning-free
  (`-Wall -Wextra -Werror`) on the full platform matrix.
- Java unit tests for symbol provider edge cases (reserved words, deep recursion, name
  collisions across namespaces).

**Docs:** `docs/design/codegen-architecture.md`; `docs/generated-types.md` (what users get per
Smithy shape — the naming/mapping contract).

**Exit criteria:** `smithy build` on the weather and cafe models emits type libraries that compile
cleanly on all CI platforms; golden tests lock the output.

---

### Phase 3 — Client generation: restJson1 + rpcv2Cbor (≈5–6 weeks)

**Goals:** generated clients are functional and protocol-compliant for **both initial protocols**
(§3.3): users can call a real service over REST or RPC.

**Tasks**
- **Protocol-generator interface** first: a `ProtocolGenerator` abstraction (mirroring
  smithy-rs's protocol layer) that owns HTTP binding/dispatch generation while delegating
  document serde to a shared, format-parameterized serde generator (JSON for restJson1, CBOR
  for rpcv2Cbor) — implementing both protocols against it is what keeps it honest.
- Operation method generation on `<Service>Client`: sync `Outcome`-returning methods first;
  async (`std::future`-based) variants once sync is stable.
- **restJson1 request serialization**: httpLabel (greedy + normal), httpQuery/QueryParams,
  httpHeader/PrefixHeaders, httpPayload (shape and blob payloads), body member serialization,
  content-type and accept headers, idempotency token auto-fill.
- **Response deserialization**: status-code handling, header/payload/body unbinding, streaming
  blob payload as `std::istream`-like body.
- **rpcv2Cbor request/response handling**: `POST /service/{Service}/operation/{Operation}`,
  `smithy-protocol: rpc-v2-cbor` header, `Content-Type: application/cbor`, whole-input CBOR body
  serialization — same generated-serde structure as restJson1 with the CBOR writer plugged in.
- **Error deserialization** (both protocols): error discriminator handling per each protocol's
  spec, generated modeled-error types, unknown-error fallback; errors surfaced through
  `Outcome::error()` with `ErrorsAs<T>()` accessors.
- Client plumbing: endpoint resolution from config, per-operation timeout override, interceptors
  invoked at generated call sites.
- **Protocol test generation**: generate GoogleTest cases from `smithy.test#httpRequestTests` and
  `#httpResponseTests` traits, run against the official suites for **both restJson1 and
  rpcv2Cbor** with a capturing mock `HttpClient`. Model this on smithy-rs's
  `ClientProtocolTestGenerator`, including its pattern of a checked-in, must-shrink **exclusion
  list** (`FailingTest`/broken-test annotations) for known-failing cases.

**Testing**
- Full restJson1 + rpcv2Cbor client protocol-test suites in CI (minus documented exclusions).
- Golden + compile tests extended to client output.
- Hand-written smoke test: generated weather client vs. the Phase 1 *hand-written* server —
  the first half of the client↔server bridge.
- Unit tests for serde helpers added to the runtime along the way.

**Docs:** `docs/client-guide.md` (quick start: model → client → first call), error-handling
guide, `examples/weather/client-demo/` runnable example.

**Exit criteria:** generated weather client passes protocol tests and talks to the hand-written
server over real sockets in CI; exclusion list reviewed and justified line-by-line.

---

### Phase 4 — Server generation: restJson1 + rpcv2Cbor (≈5–6 weeks)

**Goals:** generated servers are functional for both initial protocols: routing, deserialization,
validation, handler dispatch, response serialization.

**Tasks**
- Generate `<Service>Handler` pure-virtual interface (one method per operation, `Outcome`
  returns so handlers report modeled errors without exceptions) and `<Service>Server` that binds
  the handler to the runtime router + a `HttpServerTransport`.
- **Request deserialization** (mirror of Phase 3 serialization) and **response serialization**
  (mirror of Phase 3 deserialization) — factor shared serde generation so client and server reuse
  one serializer-generator with direction flipped.
- **Routing**: for restJson1, a generated route table (method, URI pattern, greedy labels)
  registered with the runtime router, with 404/405 handling and `Content-Type` checking (415);
  for rpcv2Cbor, dispatch on the fixed `/service/{Service}/operation/{Operation}` URI form and
  `smithy-protocol` header behind the same runtime routing interface — one server can host
  services of either protocol.
- **Constraint validation** from traits: `@required`, `@length`, `@range`, `@pattern`,
  `@uniqueItems`, enum membership — failures produce a structured 400 `ValidationException`
  *before* user handler code runs. Follow smithy-rs's server design here directly: its
  constraint-traits RFC, `ConstraintViolation` types, and `smithy.framework#ValidationException`
  wiring are the precedent, translated to C++ idioms.
- Error mapping: modeled errors → status + protocol-specific error body; unmodeled/handler
  panic → 500 with safe (non-leaking) body.
- Server-side **protocol tests**: generate tests from `httpRequestTests` (feed the wire request,
  assert parsed input) and `httpMalformedRequestTests` (assert 400s) — the malformed-request
  suite is the server's validation conformance test. smithy-rs's `ServerProtocolTestGenerator`
  is the template.
- **Generated service smoke tests** (user-facing): for every generated service, also emit a
  ready-to-run GoogleTest target (`:service_smoke_test` + BUILD entry) that spins up the
  `<Service>Server` with a generated stub handler and calls **every operation** through the
  generated client over the loopback transport — asserting routing, serde symmetry, required
  members, and modeled-error mapping end to end. Users get a passing baseline test for their
  service the moment they generate it, and a natural place to plug in their real handler.

**Testing**
- Server protocol-test + malformed-request suites for both protocols in CI with exclusion list.
- Golden + compile tests extended to server output.
- Inverse smoke test: Phase 1 hand-written client vs. **generated** server.
- Router unit tests: precedence, greedy labels, ambiguous-route detection at generation time
  (generator emits an error, with a Java unit test proving it).

**Docs:** `docs/server-guide.md` (implementing a handler, running a server, deployment notes),
validation behavior reference.

**Exit criteria:** generated weather server (restJson1) and an RPC fixture server (rpcv2Cbor)
pass server-side protocol tests; hand-written client round-trips against the weather server in CI;
the generated smoke-test target passes out of the box for every fixture model.

---

### Phase 5 — Client↔server integration testing (≈2–3 weeks)

**Goals:** the headline requirement: **generated clients integration-test generated servers**,
systematically and in CI, for every fixture model.

**Tasks**
- **Integration harness** (`tests/integration/`), an extension of the Phase 2
  generate→compile→run pipeline (smithy-rs's `codegen-client-test`/`codegen-server-test`
  pattern, taken one step further by wiring the two generated halves together):
  - For each example model, generate *both* client and server, compile them into one test binary
    with a reference handler implementation, and run GoogleTest suites that call every operation
    through the generated client against the generated server.
  - Two transport modes, same test body: (a) **loopback** (in-memory, fast, runs everywhere,
    ASan-friendly) and (b) **real sockets** on an ephemeral port (catches transport/framing bugs).
- **Round-trip coverage matrix**: auto-derive from the model a test per operation covering:
  minimal input (only required members), maximal input (all members set), boundary values for
  constrained members, every modeled error (handler triggers it, client must surface the right
  typed error), unknown-member tolerance (server adds an extra JSON field; old client must ignore
  it).
- **Property-based round-trip tests** (rapidcheck or handwritten generators): random valid inputs
  → client serialize → server deserialize → echo handler → client deserialize → compare with
  `operator==`. This is the single highest-value test for serde symmetry.
- Grow the fixture corpus to force coverage: a model exercising unions + recursive shapes, one
  with all timestamp formats and http bindings, one with pagination, one with only error cases —
  and each fixture is built in **both restJson1 and rpcv2Cbor variants** (HTTP-binding-specific
  fixtures excepted), so the integration matrix covers REST and RPC from the start.
- Nightly CI job running the full matrix with sanitizers; PR CI runs the loopback subset.

**Testing:** this phase *is* testing; its own correctness is guarded by mutation checks
(deliberately break a serializer locally → harness must fail).

**Docs:** `docs/design/integration-testing.md` — how the harness works, how to add a fixture
model, how failures are triaged.

**Exit criteria:** every fixture model has a generated-client-vs-generated-server suite green in
CI on Linux/macOS/Windows; a seeded serde bug is demonstrably caught by the harness.

---

### Phase 6 — Ease of use: tooling, packaging, docs (≈3–4 weeks)

**Goals:** a newcomer goes from empty directory to running client+server in under 15 minutes
without reading generator internals or touching Gradle.

**Tasks**
- **First-class Bazel integration** (`bazel/` + `MODULE.bazel`), targeting **Bazel 9**:
  - Rules: `smithy_cpp_client_library`, `smithy_cpp_server_library`, and `smithy_cpp_types_library`
    (attrs: `model`/`srcs` for `.smithy` files + model deps, `namespace`, `protocol`
    (restJson1 | rpcv2Cbor)), each producing an ordinary `cc_library` consumers depend on like
    any other target.
  - The generator runs as a Bazel **toolchain/action** with a hermetic JVM via `rules_java` —
    generation happens inside the build graph (correct caching, remote-execution compatible),
    never as a "run this script first" step.
  - bzlmod module `opal_cpp` published to the **Bazel Central Registry**; runtime targets
    (`@opal_cpp//runtime:core`, `:client`, `:server`, …) consumable directly.
    **Deferred**: BCR (and Maven Central) publishing waits until the project is validated in
    production; until then consumers use `git_override`/`local_path_override` (see
    docs/quickstart.md).
  - Out-of-tree consumer example (`examples/bazel-consumer/`) exercised in CI: a standalone
    Bazel 9 module that depends on the released `opal_cpp` module, defines a model, builds
    client + server, and runs the Phase-5-style integration test — this is the quick-start
    acceptance test.
- **CLI wrapper**: `smithy-cpp generate --model … --mode client|server|both --out …` (thin wrapper
  around `smithy build` with our plugin preconfigured) for generation outside Bazel — inspecting
  output, or vendoring generated sources into other environments (unsupported paths, but the
  escape hatch exists); distributed via the Smithy CLI's plugin mechanism.
- **Project template**: `smithy-cpp init` / a template repo producing the canonical Bazel 9
  module layout (model/, client/, server/, integration-test skeleton *pre-wired to test the
  server with the client*, mirroring our Phase 5 harness — users inherit the pattern for free).
- **Packaging**: Bazel Central Registry module (rules + runtime); pinned generator distribution
  via Maven Central, fetched hermetically by the rules' toolchain.
- **Docs site** (mkdocs-material or Docusaurus, published from CI): Quick Start, Client Guide,
  Server Guide, Testing Your Service, Generated Code Reference, Customization (namespaces,
  naming), Troubleshooting, FAQ. Every code snippet in the docs is extracted from compiled,
  CI-tested example code — no drifting snippets.
- Error-message audit: every generator diagnostic names the shape, the trait, and a fix.

**Testing**
- **Doc-driven acceptance test in CI**: a workflow that literally follows the Quick Start from a
  clean container (install Bazel 9 via bazelisk, init, generate, build, run the integration
  test) — if the tutorial breaks, CI fails.
- Bazel rules tested against the out-of-tree consumer module on current Bazel 9 releases
  (tracked via bazelisk pinning).
- **Follow-up**: run the consumer acceptance job on the full OS matrix (Linux/macOS/Windows),
  not just Linux.
- **Follow-up — incremental development flows**: support and document the model-evolution loop
  after initial integration: change the model (add an operation or member, tighten a
  constraint), rebuild, and let the handler-interface compile errors guide the update;
  regenerate vendored CLI output safely; keep hand-written handlers and tests working across
  regenerations. Exercise the flow in CI (scripted evolve-and-rebuild against the consumer
  example).
  ✅ Done — docs/model-evolution.md (in-graph and vendored loops, diff review, CI drift
  detection, worked example, compatibility cheat sheet);
  `examples/bazel-consumer/model-evolution-check.sh` scripts the evolve-and-rebuild loop and
  runs in the consumer CI job.

**Docs:** the docs site is the deliverable.

**Exit criteria:** quick-start acceptance test green in CI; an external tester (someone not on
the project) completes the tutorial without help; BCR + Maven Central packaging validated in CI.

---

### Phase 7 — Hardening & 0.1.0 release (≈4–6 weeks)

**Goals:** production-readiness features, robustness work, and a cut release.

**Tasks**
- **Client robustness**: retry policy (exponential backoff + jitter, honoring `@retryable`),
  connection pooling in the curl transport, request timeouts end-to-end, cancellation.
- **`@requestCompression`** (gzip request bodies): a compression codec in the runtime (zlib via
  BCR) plus client-side emission; unprunes the suite's compression operations and their
  malformed tests.
- **Server robustness**: thread-pool tuning knobs, graceful shutdown/drain, request size limits,
  slow-client timeouts, structured logging hooks, metrics hooks (request count/latency callbacks).
- **User-supplied middleware**: first-class extension seams on both sides, so auth, logging,
  tracing, and metrics are user-composable rather than one-off knobs (smithy-rs prior art:
  client interceptors + tower layers). Client side: an interceptor chain on `ClientConfig` with
  hooks around the full call and around each attempt (mutate the outgoing `HttpRequest` —
  e.g. inject auth headers — and observe the `HttpResponse`/outcome). Server side: middleware
  wrapping the transport-facing `RequestHandler` (a decorator: pre-dispatch request
  inspection/rejection, post-dispatch response observation), composing outside the generated
  router so it works with any transport. The logging/metrics hooks above and the auth hooks
  below should be built as middleware on these seams, not as parallel mechanisms.
- **Auth hooks**: `@httpBearerAuth` / `@httpApiKeyAuth` support — client-side credential
  providers, server-side authenticator interface (vendor-specific schemes such as SigV4 are out
  of scope, per §2).
- **Pagination**: generated paginator iterators from `@paginated`.
- **Observability**: SDK-free hooks enriched for real backends — server observations carry the
  matched operation name (router-stamped) and the incoming `traceparent` for log correlation;
  a client attempt-observation interceptor; W3C Trace Context helpers
  (parse/format/generate) plus a `PropagateTraceContext` client interceptor. An optional
  `//runtime:otel` adapter mapping these hooks onto opentelemetry-cpp spans/metrics is
  **post-0.1.0** (its dependency tree — protobuf, gRPC for OTLP — stays out of the dep-light
  core; stabilize the hook shapes in production first).
- **Fuzzing**: libFuzzer harnesses for JSON deserialization, URI parsing, and the server's
  request parsing (fed by the malformed-request corpus); OSS-Fuzz application once stable.
- **Performance**: benchmark suite (Google Benchmark) for serde and request throughput; publish
  numbers; no-regression check in CI (informational at first).
- **Release engineering**: semantic versioning policy, generated-code compatibility policy
  (what a generator upgrade may change), CHANGELOG discipline, signed tags, versioned docs;
  cut **v0.1.0** of runtime + generator together.

**Testing:** everything above lands with tests; fuzzers run nightly; full integration matrix
(both protocols × all fixtures × loopback+sockets × 3 OSes) green for release.

**Docs:** production guide (timeouts, retries, shutdown), upgrade/compatibility policy,
benchmark methodology.

**Exit criteria:** v0.1.0 tagged and published (BCR + Maven Central), with the release gated on
the full test matrix and the quick-start acceptance test.

---

### Phase 7e — Protocol realignment to vendor-neutral (before 0.1.0)

**Decision (2026-07):** the out-of-the-box protocols must be vendor-neutral. `restJson1` is
AWS-namespaced (`aws.protocols`), so we **drop it entirely**. Rather than invent a homegrown REST
protocol, we **adopt `alloy#simpleRestJson`** — the vendor-neutral HTTP+JSON protocol smithy4s
uses (`com.disneystreaming.alloy:alloy-core`, Maven Central). It is restJson1's binding model
with neutral error identity, and it ships an **official conformance suite**
(`alloy-protocol-tests`) as a standard `smithy.test#httpRequestTests`/`httpResponseTests` model —
which our `ProtocolTestGenerator` already consumes. Adopting it gives a documented spec, wire
interop with the smithy4s ecosystem, and a ready-made conformance suite (no test authoring), while
dropping every `aws.*` dependency. Final 0.1.0 protocol set: **`alloy#simpleRestJson` (REST/JSON) +
`smithy.protocols#rpcv2Cbor` (RPC/CBOR) + `smithy.cpp.protocols#jsonRpc2` (RPC/JSON)** — all neutral.

`simpleRestJson` error wire format (verified against alloy's own conformance model): modeled
error → status from `@httpError`, response header **`X-Error-Type: <ShapeName>`** (name without
namespace), body = the error structure's members as plain JSON. **No `__type` body field** — the
header is the sole discriminator; the client falls back to the `@httpError` status when the header
is absent. Deltas from restJson1: (1) header `x-error-type` not `x-amzn-errortype`, (2) no `__type`
in the error body, (3) client status-code fallback.

**Tasks**
- **`simpleRestJson` protocol generator**: `SimpleRestJsonProtocol` targeting `alloy#simpleRestJson`,
  reusing the shared HTTP+JSON binding code (now a base class extracted from `RestJson1Protocol`).
  Protocol hooks carry the three error deltas above. Add `alloy-core` as a codegen dependency.
- **Adopt the alloy conformance suite**: add `alloy-protocol-tests`; generate a simpleRestJson
  protocol-test module the same way as the restjson1/rpcv2cbor suites; drive to green.
- **Remove `restJson1`**: delete from `resolveProtocol`, drop `smithy-aws-traits` +
  `smithy-aws-protocol-tests` — no `aws.*` on the classpath.
- **`jsonRpc2` protocol** (homegrown; JSON-RPC 2.0 is an open standard, alloy has no equivalent):
  `@protocolDefinition` trait `smithy.cpp.protocols#jsonRpc2`; single POST endpoint,
  `{"jsonrpc":"2.0","method":<operation>,"params":<input>,"id":…}` request and
  `{"jsonrpc":"2.0","result":<output>,"id":…}` / `{"error":{code,message,data}}` response over the
  `opal::Document` JSON pivot; JSON-RPC error objects map to modeled errors (`data` carries the
  shape). We author its protocol-test model (no external suite exists).
  ✅ Done — `JsonRpc2Protocol` (client + server; the single-endpoint dispatch overrides the new
  `ProtocolGenerator.writeServerRoutes` hook), authored conformance suite under
  `protocol-tests/jsonrpc2` (request/response/error/malformed cases, all green). Wire contract:
  every JSON-RPC envelope travels as HTTP 200; modeled errors carry their `@httpError` status as
  the application `error.code` with the fully qualified shape id in `data.__type`; the reserved
  codes cover envelope-level failures (-32700/-32600/-32601/-32602); validation mirrors rpcv2Cbor
  (`smithy.framework#ValidationException` + `fieldList`, code 400). The normative definition lives
  on `JsonRpc2Protocol`'s class docs.
- **Migrate fixtures/examples**: weather + roundtrip REST move to `simpleRestJson`; add a `jsonRpc2`
  fixture. Consumer example overlays become `simplerestjson` / `rpcv2cbor` / `jsonrpc2`.
  ✅ Done — `RoundTripJsonRpc` serves the kitchen sink over JSON-RPC (generated integration suite
  green), `examples/jsonrpc2` adds a classic calculator service with a hand-rolled-peer interop
  wire test (foreign string ids, raw envelopes both directions), and the out-of-tree consumer
  gained the `jsonrpc2` overlay + a third integration-test leg.

**Sequencing:** lands after Phase 7d fuzzing/benchmarks and before the 0.1.0 tag — no point
cutting a release on a protocol surface we are about to replace. Release engineering (versioning,
CHANGELOG, tag) is the last step, on the finalized neutral protocol set.

### Phase 8 — Bidirectional streaming (post-0.1.0, ≈6–8 weeks)

**Goals:** first-class streaming operations: `@streaming` blob streams and event-stream unions,
including **bidirectional** streams, over HTTP and **WebSocket** transports, generated for both
client and server — and integration-tested the same way as everything else: generated streaming
clients drive generated streaming servers in CI.

**Tasks**
- Design doc + ADR first: streaming API shape for C++ (`opal::EventStream<Tx, Rx>` with an
  async sender/receiver pair; backpressure semantics; cancellation; relationship to the
  `std::future`-based unary API and whether this is the point to introduce a coroutine API).
- Runtime: event-stream message framing as mandated by the protocol specs (the
  `application/vnd.amazon.eventstream` framing that restJson1/rpcv2Cbor event streams are
  defined against — a wire-format requirement of the protocols, not an AWS service dependency);
  WebSocket transport implementations (client + server, building on the Phase 1 upgrade hooks);
  flow control and half-close handling.
- Codegen: event-stream union member types, streaming operation signatures on client and
  handler interfaces, initial-request/initial-response handling, modeled errors delivered
  mid-stream.
- Testing, in the established pattern: framing codec unit tests + fuzzer; protocol tests where
  the official suites cover event streams; and Phase-5-harness extension — streaming fixtures
  (echo stream, server-push, client-push, full duplex chat) where the **generated client
  integration-tests the generated server** over loopback, HTTP, and WebSocket transports, with
  property-based round-trips of random event sequences.
- Docs: streaming guide (client + server), transport selection, backpressure/cancellation
  semantics reference.

**Exit criteria:** a full-duplex chat example (generated client ↔ generated server over
WebSockets) runs in CI on all platforms; streaming fixtures green in the integration matrix;
framing fuzzer running nightly.

**Progress notes:** the exit criterion is met (the chat example, ADR-0016); the async
serving surface shipped as ADR-0019/0021, reconnect grace and admission as ADR-0020/0022,
the browser wire as ADR-0018 — and streams now cover all three protocols: jsonRpc2's
JSON-RPC-native wire landed as ADR-0023 (issue #123), retiring the generation-time
refusal that slice 3 shipped with.

---

## 5. Testing strategy (summary)

| Layer | What | Where it lands |
|---|---|---|
| Runtime unit tests | GoogleTest on core/json/http/client/server modules, ≥90% coverage on core | Phase 1, continuous |
| Codegen unit tests | Java tests for symbol provider, naming, route-conflict detection | Phase 2+ |
| Golden (snapshot) tests | Byte-exact expected generated output per fixture model | Phase 2+ |
| Compile tests | Generated code must build `-Werror` on Linux/macOS/Windows | Phase 2+ |
| Protocol compliance | Generated tests from Smithy `httpRequestTests`/`httpResponseTests`/`httpMalformedRequestTests` (client- and server-side), official restJson1 + rpcv2Cbor suites | Phases 3–4 |
| **Client↔server integration** | **Generated client exercises generated server** per fixture: coverage matrix + property-based round-trips, loopback + real sockets | Phase 5, continuous |
| Doc acceptance | CI follows the Quick Start verbatim from a clean environment | Phase 6 |
| Fuzzing & sanitizers | ASan/UBSan on all C++ tests; libFuzzer on parsers, nightly | Phase 0 (sanitizers), Phase 7 (fuzzing) |
| Benchmarks | Serde + throughput, regression-tracked | Phase 7 |

---

## 6. CI matrix (steady state)

- **Per-PR:** Bazel builds+tests on Linux gcc/clang, macOS, and Windows MSVC; runtime unit
  tests; codegen tests + goldens; compile tests; protocol tests (both protocols); loopback
  integration suite; lint/format (incl. buildifier).
- **Nightly:** full integration matrix (sockets + sanitizers), fuzzers, coverage report,
  benchmark run, doc acceptance test, out-of-tree Bazel consumer on current Bazel 9 releases.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| JVM dependency deters C++ users | Build-time only; hidden behind the Bazel rules' hermetic toolchain and the CLI (Phase 6); documented clearly in ADR-0001 |
| Protocol edge cases (timestamps, escaping, greedy labels) | Lean on official protocol-test suites from day one; exclusion lists are visible, reviewed, and must shrink |
| Serde asymmetry between client and server | Single shared serde generator with direction flipped (Phase 4); property-based round-trip tests (Phase 5) |
| Golden tests become churn-heavy | Deterministic output requirement + one-command regeneration + goldens reviewed as normal diffs |
| C++ dependency pain (curl/Beast) for consumers | Transports isolated behind interfaces; header-only loopback needs no deps; BCR modules handle the rest |
| Bazel-only excludes non-Bazel users | Deliberate scope decision (ADR-0004); the CLI still emits plain C++ sources + a manifest that can be vendored into any build, as an unsupported escape hatch |
| Windows quirks (winsock, MSVC conformance) | Windows in the CI matrix from Phase 0, not retrofitted |
| Scope creep toward AWS SDK features | Non-goals section + ADRs; auth limited to bearer/api-key in 0.1 |

---

## 8. Deferred beyond 0.1.0

**Bidirectional streaming is deferred but planned — it is Phase 8, not an open-ended deferral.**
Beyond that: HTTP/2 & TLS configuration surface, additional protocols (added only on demand
behind the existing protocol interface), waiters, MQTT bindings, C++ modules support — each
gets an issue and a design doc before implementation. ~~Async server handler API
(coroutines)~~ — resolved: shipped as the generated async streaming handlers (ADR-0021); the
async generated *client* remains deferred.
Vendor-specific traits and auth schemes remain out of scope entirely (§2), not deferred.

## 9. Open questions (to resolve in early ADRs)

1. Serde backends: nlohmann (ergonomics) vs simdjson+writer (speed) for JSON; an existing CBOR
   library vs a small hand-rolled codec for rpcv2Cbor — start simple behind the format-agnostic
   serde interface, benchmark in Phase 7 (ADR in Phase 1).
2. Async model for 0.1: `std::future` now, coroutine (`co_await`) client in a later minor —
   confirm in Phase 3 design; revisit when designing the Phase 8 streaming API.
3. Minimum compiler floor: proposal gcc 11 / clang 14 / MSVC 19.30 — confirm in Phase 0.
4. ~~Whether server transport ships Beast-only or also a plain-asio minimal HTTP/1.1 impl.~~
   **Resolved (ADR-0006):** Boost.Beast via the BCR's modular `boost.beast` is the supported
   server transport (asio concurrency, TLS path via `boringssl`, `beast::websocket` for
   Phase 8); libcurl (BCR) remains the planned production client transport. The Phase 1
   built-in socket transport is retained as a zero-dependency test/reference transport only.

---

## 10. Milestone summary

| Phase | Outcome | Indicative duration |
|---|---|---|
| 0 | Repo, CI, ADRs, fixture models | 1–2 wk |
| 1 | Runtime library; hand-written client+server round-trip | 3–4 wk |
| 2 | Plugin + type generation, golden & compile tests | 3–4 wk |
| 3 | Generated client, restJson1 + rpcv2Cbor, protocol tests | 5–6 wk |
| 4 | Generated server (both protocols), validation, protocol tests | 5–6 wk |
| 5 | Generated-client ↔ generated-server integration harness | 2–3 wk |
| 6 | Bazel 9 rules/UX, CLI, packaging (BCR + Maven Central), docs site, quick-start acceptance test | 3–4 wk |
| 7 | Hardening, fuzzing, **v0.1.0** | 4–6 wk |
| 8 | Bidirectional streaming: event streams + WebSockets, client & server (post-0.1.0) | 6–8 wk |

Total: roughly 7–9 months of focused work for a small team to v0.1.0, plus Phase 8 streaming
after it, with a usable generated-client-tests-generated-server demo available from the end of
Phase 5.
