# Integration testing: generated clients vs. generated servers

Phase 5's headline requirement: **generated clients integration-test generated servers**,
systematically, for every fixture model. Every module generated with `--integration-tests true`
ships `tests/integration_test.cc` (Bazel target `:integration_test`), in which the module's own
generated client calls its own generated server through real transports.

## How a suite works

The generated fixture is parameterized over two transports with identical test bodies:

- **Loopback** — in-memory `opal::http::Loopback`; fast, deterministic, sanitizer-friendly.
- **Socket** — `opal::http::SocketHttpServer` bound to an ephemeral port on 127.0.0.1 with the
  client pointed at it; catches transport/framing bugs the loopback can't.

A generated `ScriptedHandler` records the input each operation received and answers with a
scripted output (or error), so both wire directions are asserted independently:

1. client serialize → server parse: `handler->lastOp == input`
2. server serialize → client parse: `*outcome == output`

## The coverage matrix

Per operation, the generated suite contains:

| Row | What it does |
|---|---|
| `<Op>RandomRoundTrips` | 8 seeded random inputs/outputs round-trip exactly (`operator==`) |
| `<Op>MaximalRoundTrips` | every optional member set, containers at max size |
| `<Op><Error>MapsAcrossTheWire` | one per modeled error: a random typed detail must survive the wire (code, kind, and full detail equality) |
| `<Op>ToleratesUnknownResponseMembers` | a `MutatingTransport` injects an unknown member into the response body; the client must ignore it (documents-map bodies only) |

## Random values (`RandomValueGenerator`)

`Random<Shape>(Rng&)` builders are generated per aggregate shape from the same model the serde
comes from. Two properties matter:

- **Constraint-valid**: values always satisfy `@length`/`@range`/`@pattern`/`@uniqueItems`/enum
  membership — the server validates before the handler runs, so an invalid input would 400.
  Pattern-constrained strings use a fixed Java-checked candidate; numbers respect range bounds
  (boundary-biased: min/max come up 10% of the time each); `@uniqueItems` string lists get
  distinct suffixes; `@idempotencyToken` members are always set (the client would auto-fill an
  unset one and break request equality).
- **Wire-exact**: values stay inside the subset that round-trips exactly — alphanumeric strings
  (no header/URI escaping edge cases), whole-second timestamps (http-date has second precision),
  dyadic-fraction floats (exact in JSON/CBOR and both widths), non-empty engaged containers
  (engaged-empty header/query lists don't survive), and non-empty blobs (an empty optional blob
  payload reads back as unset).

The `Rng` is seeded `std::mt19937`, so any failure reproduces deterministically.

## Fixtures

| Module | Protocol | Coverage |
|---|---|---|
| `examples/weather` | simpleRestJson | labels, query, resources, errors, 204 |
| `examples/cafe` | rpcv2Cbor | unions (incl. Unit members), documents-free CBOR |
| `examples/simplerestjson` | simpleRestJson | the bookstore quick-start service |
| `examples/roundtrip` (rest) | simpleRestJson | kitchen sink: every binding location at once, all three timestamp formats, sparse/unique lists, maps, unions, blob payload + struct payload, prefix headers, documents, both error classes |
| `examples/roundtrip` (rpc) | rpcv2Cbor | the same kitchen-sink shapes over CBOR |
| `examples/roundtrip` (jsonrpc) | jsonRpc2 | the same kitchen-sink shapes over JSON-RPC 2.0 envelopes |
| `examples/jsonrpc2` | jsonRpc2 | calculator showcase + hand-rolled-peer interop wire tests; stream e2e on both seams over the pair and real WebSockets, pinning the browser-shaped JSON-RPC text (ADR-0023) |

`examples/roundtrip/model/roundtrip.smithy` defines all three services over shared shapes, so
the REST and RPC matrices exercise identical structures. To add a fixture: write the model, register
a `registerFixtureTask` in `codegen/smithy-cpp-codegen/build.gradle.kts` (the
`--integration-tests true` flag is on for all fixture tasks), regenerate, and the suite exists.

## CI and mutation checking

`bazel test //...` runs every integration suite — including the socket mode — on Linux
(gcc + clang), macOS, and Windows on every PR, plus a clang ASan+UBSan job. The harness's own
correctness is guarded by mutation checks: deliberately corrupting a generated serializer (e.g.
`1 + value.precise` in the roundtrip serde) makes the suite fail on the spot; this was verified
when the harness landed and should be re-verified when the round-trip machinery changes.

## Triaging a failure

Failures name the operation, the direction (`handler->lastOp` mismatch = request path,
`*outcome` mismatch = response path), and the transport (test suffix `/Loopback` or `/Socket`).
A socket-only failure is a transport/framing bug; a both-transport failure is serde or bindings.
Seeds are fixed in the generated file, so rerunning the one test reproduces the exact values.
