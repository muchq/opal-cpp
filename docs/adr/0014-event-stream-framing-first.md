# ADR-0014: Phase 8 lands wire-format-first — the event-stream framing codec before any transport or API surface

**Status:** Accepted (2026-07-19)

## Context

Phase 8 (PLAN §Phase 8) is bidirectional streaming: `@streaming` blob
streams and event-stream unions, client and server, over HTTP and
WebSocket. It is the largest remaining feature and it cannot land as one
change: it spans a wire format, two transports, the public streaming API,
and generated signatures on both the client and handler interfaces. The
plan's own first task is the design decision, so this ADR records the
slicing and the first slice's decisions; API-surface details that later
slices own are recorded as direction, not contract.

The slicing question is where to start. Three candidates:

1. **The public API first** (`EventStream<Tx, Rx>` types with a loopback
   pump): visible progress, but every decision made without the wire
   format and transports underneath is a guess that hardens into API.
2. **WebSocket transport first**: concrete, but with no streaming API or
   framing to carry, a WS upgrade path has no consumer-facing surface to
   prove itself against — it would ship as dead code or grow an ad-hoc
   raw-frame API that slice 3 would have to break.
3. **The wire format first**: the event-stream message framing
   (`application/vnd.amazon.eventstream`) that restJson1-family and
   rpcv2Cbor event streams are *defined against* — a wire-format
   requirement of the protocol specs, not an AWS service dependency
   (PLAN §Phase 8). Everything later stacks on it; nothing about it
   depends on the undecided layers above.

## Decision

**Wire-format-first.** Phase 8 lands in slices, each independently
shippable and tested in the house pattern:

1. **This slice: the framing codec** — `opal/eventstream/frame.h`, a
   hand-rolled encoder/decoder for the event-stream message format, in
   the same mold as the CBOR codec and the extracted HTTP/1 parser:
   pure functions over buffers, no I/O, hostile-input test bank, libFuzzer
   harness in the CI smoke loop.
2. **WebSocket transports** (server upgrade on `BeastServerTransport` via
   the ADR-0006 extension point, client dial), carrying framed messages —
   with `on_connection_event` (ADR-0013) extended to upgrade failures as
   a new Kind if slice 2 finds the need. Definition of done includes an
   out-of-tree consumer e2e in `examples/bazel-consumer` (a consumer
   dials the upgraded server and drains real frames through the module
   boundary), the way slice 1 shipped `eventstream_consumer_test` — not
   just in-repo transport tests.
3. **The streaming API + codegen**: `opal::EventStream<Tx, Rx>` as a
   blocking sender/receiver pair mirroring the sync unary API (recorded
   here as *direction*: no coroutine surface before 1.0 — a coroutine
   adapter can wrap a blocking pair, not vice versa; backpressure by
   bounded queue; cancellation by close), generated streaming-operation
   signatures, initial-request/initial-response, mid-stream modeled
   errors. The API ADR lands with that slice, informed by two working
   layers beneath it.

Codec decisions pinned now:

- **Message model:** `Message{headers, payload}`; a header is a name plus
  one of the format's ten wire types (bool true/false collapse into one
  C++ `bool` alternative; timestamps and UUIDs get distinct value types so
  they cannot be confused with `long`/byte-array — the timestamp is the
  runtime's one `opal::Timestamp`, so slice-3 generated code trades
  timestamps with headers without a conversion). The payload is a
  `opal::Blob` — the framing layer carries protocol bytes, it does not
  interpret them.
- **Hand-rolled, dependency-free:** big-endian packing and CRC32 (the
  IEEE/zlib polynomial) are written out in `frame.cc` like the CBOR
  codec's primitives — no zlib/absl dependency for one table and two
  loops.
- **Strict decode, fail-closed:** both CRCs verified (prelude before any
  length is trusted, message before any content is surfaced); unknown
  header wire types, header blocks that overrun their declared length,
  and out-of-bounds declared lengths are hard errors, not skips. An
  incomplete buffer is `nullopt` ("feed me more"), never an error — the
  decoder is incremental because every transport will feed it from a
  socket.
- **Bounded:** header block ≤ 128 KiB, total message ≤ 16 MiB (the
  format's own documented limits). Encode refuses what Decode would
  reject — a message that cannot round-trip cannot be produced.
- **Vendor neutrality holds:** this is the framing the protocol specs
  mandate for event streams, implemented from the wire-format description;
  nothing AWS-specific rides along (no SigV4 chunk signing — that is
  auth-trait territory and out of scope per PLAN non-goals).

## Consequences

- Slice 2/3 build on a fuzzed, hostile-banked codec instead of debugging
  framing and transport simultaneously; the codec's limits become the
  streaming API's natural message-size contract.
- `//runtime:eventstream` is a leaf library (`:core` only), so consumers
  pay for it only when they use it, and the local no-boost test path
  covers it fully.
- The README's `@streaming` limitation stands until slice 3; the
  changelog and runtime docs point at this ADR for the slicing.
- The `EventStream<Tx, Rx>` direction recorded here constrains slice 3's
  ADR only as far as written: blocking pair, bounded-queue backpressure,
  close-as-cancellation. Everything finer is that ADR's to decide.
