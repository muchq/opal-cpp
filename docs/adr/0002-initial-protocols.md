# ADR-0002: Ship restJson1 and rpcv2Cbor from the start

**Status:** Superseded by Phase 7e (2026-07) on the REST protocol — see below.

> **Superseded (Phase 7e):** `restJson1` was used through Phases 3–7 for its mature official
> conformance suite, but it lives in the AWS `aws.protocols` namespace, contradicting the
> vendor-neutrality goal. The REST protocol is now **`alloy#simpleRestJson`** (the neutral
> protocol smithy4s uses; from `com.disneystreaming.alloy:alloy-core`), conformance-tested with
> `alloy-protocol-tests`. `rpcv2Cbor` (this ADR's RPC choice, already neutral) is unchanged. No
> `aws.*` remains on the generator classpath. Phase 7e also added the third protocol,
> **`smithy.cpp.protocols#jsonRpc2`** (JSON-RPC 2.0 over a single POST endpoint; homegrown trait
> since neither alloy nor core Smithy covers it, with an authored conformance suite under
> `protocol-tests/jsonrpc2`). See docs/PLAN.md §"Phase 7e".

## Context

Smithy protocols are pluggable. We need to choose which protocols the first generator versions
implement, for both clients and servers. A single initial protocol risks baking that protocol's
assumptions into the "abstraction"; deferring RPC support was rejected during plan review.

## Decision

Implement **two protocols in the same phases** (PLAN Phases 3–4):

- **`aws.protocols#restJson1`** — REST: full HTTP binding surface (labels, query params,
  headers, payloads, status codes). It lives in the `aws.protocols` namespace for historical
  reasons but is the de-facto standard protocol for generic Smithy REST services and carries no
  AWS coupling beyond protocol-mandated names.
- **`smithy.protocols#rpcv2Cbor`** — RPC: the vendor-neutral protocol in the core Smithy
  namespace (`POST /service/{Service}/operation/{Operation}`, CBOR bodies).

Different bindings *and* different wire formats (JSON vs CBOR) force the protocol-generator and
serde abstractions to be genuinely format-agnostic from day one. Both protocols have official
conformance suites (`smithy-aws-protocol-tests`, `smithy-protocol-tests`) that we generate tests
from.

## Consequences

- The runtime carries both a JSON and a CBOR serde module behind one reader/writer interface
  (Phase 1).
- Further protocols slot in behind the same `ProtocolGenerator` interface later, added on
  demand (JSON-RPC 2.0 landed in Phase 7e as `smithy.cpp.protocols#jsonRpc2`; restXml remains
  a candidate).
- opal-cpp stays vendor-neutral: no AWS traits, auth, endpoint logic, or SDK behaviors (PLAN §2).
