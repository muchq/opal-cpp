# ADR-0023: jsonRpc2 event streams — the JSON-RPC-native wire

**Status:** Accepted (2026-07-21). Issue #123; amends ADR-0016's scope
decision ("jsonRpc2 refuses them"), which was an explicit deferral until
a consumer needed a story. Implemented: the raw-text transport mode, the
`JsonRpcStreamSocket` translation wrapper and `jsonrpc_frame` codec
(`//runtime:eventstream_jsonrpc`), `ReceiveMessage`, and the
`JsonRpc2Protocol` streaming emitters. The authored conformance suite
under `protocol-tests/jsonrpc2` is normative for the wire, per the
ADR-0002 precedent.

## Context

jsonRpc2 is the vendor-neutral JSON protocol — the natural binding for
exactly the browser-facing services ADR-0018 built the JSON-text wire
for — and it is the only protocol that refuses `@streaming` unions at
generation time. One streaming operation forces a whole service onto
simpleRestJson or rpcv2Cbor. Everything below the protocol layer already
exists and is protocol-agnostic: `EventStream`/`AsyncEventStream`
(ADR-0016/0019/0021), `WebSocketRouter` + `SessionRegistry` (ADR-0017,
ADR-0022), Origin gating (ADR-0018).

Issue #123 decided the wire after a prior-art survey (go-ethereum,
jsonrpsee, LSP, graphql-ws — unanimous that JSON-RPC streams are
notifications on one duplex envelope channel, not a second framing).
This ADR pins that wire exactly and records where each piece of the
implementation lives.

## The wire

- **Upgrade.** One WebSocket upgrade `GET` on the protocol's shared
  endpoint — the same `<path-prefix>/` the unary `POST` uses. No
  subprotocol, no per-operation URI: routing stays where jsonRpc2 puts
  it, in the envelope. Every message, both directions, is a **text**
  frame carrying one JSON-RPC 2.0 envelope; a binary frame is a
  protocol violation that fails the session (the ADR-0018 posture,
  transposed). The upgrade rides the ordinary admission chain — gate
  (`RequireOrigin`, tickets), auth, router — unchanged.
- **Open.** The client's first message is an ordinary request envelope:
  `{"jsonrpc": "2.0", "method": "<OperationName>", "params": {...},
  "id": 1}`. `method` is the operation's shape name — the same naming
  the unary dispatch uses — and selects the operation. `params` carries
  the operation's **initial-request members** (the input members outside
  the stream union), serialized exactly as the unary protocol would
  serialize them; `{}` (or an absent/null `params`, per the unary
  tolerance) when there are none. This realizes the `:initial-request`
  seam ADR-0016 reserved: jsonRpc2 is the first protocol carrying
  body-bound initial-request members. `id` is `1`, the unary constant —
  one stream per socket makes it a formality, echoed rather than
  meaningful. Unlike the unary endpoint, the id is **mandatory**: a
  notification-shaped opening (no id) is refused with `-32600`
  (`"the opening call must carry an id"`) — nothing could answer it and
  nothing could echo it.
- **Events.** Each event, in both directions, is a JSON-RPC
  **notification**: `{"jsonrpc": "2.0", "method": "<eventMember>",
  "params": {"id": <opening id>, "payload": {...}}}`. `method` is the
  stream union's member name (the `:event-type` discrimination of
  ADR-0016, transposed — the same move ADR-0018 made); `payload` is the
  event structure serialized as a JSON object; `id` echoes the opening
  call's id inside `params` (the `eth_subscribe` shape). The echo costs
  a few bytes now and leaves the door open to multiplexed streams later
  without a wire break; nesting under `params` keeps modeled member
  names collision-free. One event per text message — the
  one-message-one-event rule verbatim.
- **Terminal.** The stream ends with a proper **response envelope for
  the opening id**, then a server-initiated close. Clean handler
  completion answers `{"jsonrpc": "2.0", "result": {}, "id": 1}` —
  `result` is the empty object while ADR-0016's initial-response
  deferral stands, and is where initial-response members ride when that
  lands. A modeled exception answers the protocol's existing error
  object unchanged: `{"jsonrpc": "2.0", "error": {"code":
  <@httpError-derived, 400 default>, "message": "...", "data": {...,
  "__type": "<ns#Shape>"}}, "id": 1}` — byte-for-byte the unary
  `JsonRpcError` convention. A received exception is terminal, exactly
  the ADR-0016 contract.
- **Reserved codes.** Envelope-level failures use the unary endpoint's
  reserved codes as a terminal error, then close: `-32700` unparseable
  JSON, `-32600` not a valid request envelope, `-32601` unknown
  method — including a method that names a *unary* operation: the
  stream endpoint dispatches streaming operations only, mirroring the
  unary `POST` endpoint, which knows no streaming methods and answers
  them `-32601` too — and `-32602` params that fail
  **deserialization**. Params that deserialize but fail *constraint
  validation* answer the unary validation identity instead: the `400`
  error object carrying `smithy.framework#ValidationException`, exactly
  what the unary endpoint answers, before any handler runs. A vanilla
  JSON-RPC 2.0 client that ignores notifications observes a well-formed
  call → response pair in every case.
- **Mid-stream policing.** After a live opening, `JsonRpcStreamSocket`
  (see Decisions) classifies every inbound frame; an envelope-level
  violation is **session-fatal on both ends**. The classifications, each
  with a fixed reason string pinned by the conformance suite (no decoder
  detail leaks into wire text): `-32700` for a text frame that is not
  JSON; `-32600` for everything else the envelope grammar refuses — a
  non-object envelope, a wrong or missing `jsonrpc` version, an unknown
  envelope or params member, a request envelope after the opening call,
  a notification without the opening id echo (one stream per socket), a
  response for a foreign id, a malformed error object, an oversized
  frame, and **any response envelope sent by the client** (terminals are
  server-minted; a client-sent `result` must not read as the peer's
  clean close). The server end answers the reserved-code terminal error
  for the opening id — `{"error": {"code": <code>, "data": {"__type":
  "SerializationException"}, "message": "<reason>"}, "id": <opening
  id>, "jsonrpc": "2.0"}` — *before* its close (the close and the
  caller's completion both ride the send's completion, so nothing can
  cancel the write); the client end just fails closed. Both surface
  `Error::Serialization` to the layer above. **Carve-out:** a
  grammatically valid notification whose *modeled* content is wrong — an
  unknown event member name, a payload the generated decoder refuses —
  fails at the modeled layer, where ADR-0016's terminal rule applies
  unchanged: the session closes, but the shared decode machinery closes
  the socket before the driver's terminal send, so a terminal envelope
  is **not guaranteed** on that path (the binary wires behave
  identically; the conformance suite pins the distinction).
- **One stream per socket.** Unary calls never share the socket; the
  session stays per-operation, so the ADR-0015/0017 backpressure,
  close, and fanout contracts apply unchanged.

## Decisions

- **The transport learns raw text, not JSON-RPC.** The Beast session
  grows a third wire mode beside binary and ADR-0018's json-frames: in
  raw-text mode a headerless `eventstream::Message` rides as a verbatim
  text frame (payload bytes = frame bytes), a received text frame
  arrives as a headerless `Message`, and `Send` refuses a message
  carrying headers — encode refuses what the wire cannot represent
  (ADR-0014's rule, third application). Server side the mode is
  `Options::websocket_raw_text_frames`, mutually exclusive with
  `websocket_accept_json_frames` (one listener, one wire family) and
  refused without a websocket mount, like the ADR-0018 flag; there is
  no negotiation because the wire *is* the protocol — only a jsonRpc2
  streaming server sets it, and everything it serves speaks it. Client
  side `WebSocketDialRequest` grows the matching flag, set by generated
  code on every stream dial. The in-memory pair needs nothing:
  `Message` values cross by value, so headerless raw-text messages ride
  it today.
- **The translation is a wrapper socket, not a session flag.** ADR-0018
  mounted its translation inside the Beast session, which the in-memory
  pair never sees. The JSON-RPC translation instead lives in
  `opal::eventstream::JsonRpcStreamSocket`, a delegating
  `http::WebSocket` (the `DialedWebSocket` shape) worn by **both ends,
  on both seams, over both transports**: `Send`/`SendAsync` render an
  event-envelope `Message` as a notification (headerless raw-text
  `Message` beneath); `Receive`/`ReceiveAsync` parse inbound text and
  classify — a notification becomes exactly the event `Message` the
  binary wire would have carried (`:content-type: application/json`
  stamped), an `error` response becomes the exception `Message`
  (`:exception-type` from `data.__type`, payload = `data`), a `result`
  response for the opening id is the clean end of stream (`Receive`
  reports stream end; the terminal result never surfaces as an event).
  `EventStream`, `AsyncEventStream`, the routers, `SessionRegistry`,
  and the generated stream classes are untouched — and because the
  wrapper runs above the transport, the pair exercises the **actual
  JSON text** in every integration test, which ADR-0018's mode never
  could. The stateless envelope grammar lives in a `jsonrpc_frame`
  codec pair beside it (`EncodeJsonRpcNotification` /
  `DecodeJsonRpcStreamFrame`), the one place the member names are spelled —
  `json_frame.h`'s rule, applied again — scoped honestly: the codec
  spells the **mid-stream** grammar only; the opening and terminal
  envelopes are generated code's (see below), their spellings living in
  the emitters, with the conformance suite pinning both. Fail-closed
  transposes as classification, not error: everything the grammar
  refuses comes back as a violation frame carrying the reserved code
  and a fixed reason, which the wrapper polices per the mid-stream
  rules above. Two recorded classification edges: the exception type is
  read from `data.__type` **verbatim**, so a jsonRpc2 terminal carries
  the fully qualified shape id where the binary wires stamp the short
  name — both spellings resolve through the generated decoders'
  `SanitizeErrorCode`, and the wrapper never respells either; and an
  error object carrying no `data.__type` falls back to the type
  `"JsonRpcError"`, which matches no modeled shape and surfaces as the
  generic terminal error — a service that actually models a shape named
  `JsonRpcError` would collide with the fallback (accepted; the name
  is reserved by convention).
- **Envelope construction stays generated; the split of duties mirrors
  unary.** The unary precedent is exact: the runtime routes, generated
  code speaks JSON-RPC. The generated client builds the opening
  envelope (it owns the initial-request serde) and sends it on the
  dialed socket before wrapping it and returning the stream — the
  terminal-response handling surfaces as the stream's `Outcome` through
  the wrapper's classification. The generated server parses and
  validates the opening envelope, answers reserved-code failures, and
  builds the terminal `result`/`error` envelopes (it owns the output
  serde and the `@httpError` table). The wrapper owns only what is
  protocol-fixed: notifications and inbound classification.
- **Dispatch-on-first-message is generated code behind one route,
  not a router mode.** The stream routers keep their URI grammar; the
  jsonRpc2 server registers a single `"/"` stream route for the whole
  service — the plural-hook move `writeServerRoutes` already made for
  unary `POST "/"`, applied to `writeStreamServerRoutes` /
  `writeStreamSessionRoutes` — and the route body reads the opening
  envelope, validates it, and dispatches on `method` among the
  service's streaming operations. The blocking seam reads it with
  `socket.Receive()`; the session seam awaits the new
  `opal::eventstream::ReceiveMessage(socket)` — the single-shot
  receive twin of ADR-0021's `SendMessageAwaitable`, added to the
  runtime because a Detached launch body has no other way to read one
  message before constructing the stream. Unknown method, bad
  envelope, bad params: the route answers the reserved-code terminal
  error over the same awaitable machinery and closes, before any
  handler runs.
- **Validation gains the body-bound mode it refused.**
  `EventStreamCodeGen.validate` drops the jsonRpc2 refusal;
  `validateInitialRequestMembers` grows the third mode the protocol
  needs — a protocol that carries initial-request members in the
  opening envelope's `params` (`bindsInitialRequestMembers` stays the
  binding-protocols predicate; jsonRpc2 answers a new
  body-carriage predicate instead). The binding protocols keep their
  existing restriction — body-bound members remain refused there, with
  the diagnostic now pointing at jsonRpc2 as the protocol that carries
  them. Everything else validate enforces — `@eventHeader`/
  `@eventPayload` rejection, the initial-response deferral — applies to
  jsonRpc2 unchanged.

## Alternatives rejected

- **The framed envelope** (synthesized per-operation upgrade URIs +
  ADR-0014/0016 binary framing with JSON payloads): rejected in issue
  #123 after the prior-art survey. It would bolt URI routing onto a
  protocol whose documented contract is "no HTTP bindings apply", leave
  initial-request members with nowhere to go, introduce a second error
  identity (`:exception-type` headers beside the JSON-RPC error
  object), and tax every consumer of the open-standard protocol with a
  smithy-specific codec forever. The native wire's cost is one-time
  work here instead: the raw-text mode, the wrapper, and this suite.
- **JSON-RPC translation inside the Beast session** (the ADR-0018
  mount): invisible to the in-memory pair, so the wire would be pinned
  only by Beast-backed tests; stateful (the opening id) and
  code-mapping-aware (the `@httpError` table) where the transport is
  deliberately dumb. The wrapper keeps the transport's three modes
  mechanical.
- **A router dispatch-on-first-message mode** (issue #123's work-list
  sketch): the router would have to read, parse, and answer JSON-RPC —
  protocol knowledge the router has never held for any protocol; the
  unary precedent (envelope dispatch in the generated `POST "/"` body)
  already places it in generated code, and one `"/"` stream route
  gives the router nothing to disambiguate.
- **Subprotocol-negotiated raw text**: the ADR-0018 negotiation exists
  for a shared endpoint serving two wire encodings to different
  clients. A jsonRpc2 stream endpoint serves exactly one; a browser
  should connect with `new WebSocket(url)` and nothing else. The flag
  is configuration, not negotiation.
- **Multiplexed streams** (several calls sharing a socket, demuxed by
  id): the id echo deliberately leaves the door open, but ADR-0017's
  session, backpressure, and fanout contracts are per-socket today;
  multiplexing is a real design, not a freebie, and no consumer has
  asked.

## Consequences

- A jsonRpc2 streaming service is browser-consumable with `JSON.parse`
  alone — no subprotocol token, no codec, no build step; admission is
  the ADR-0018 pattern (ticket + `RequireOrigin`) unchanged.
- `//runtime:eventstream_jsonrpc` joins `eventstream_json` beside the
  binary codec; `//runtime:http_beast` learns the third wire mode but
  no new dependencies (raw text needs no codec at all). The pair stays
  wire-free and single-code-path.
- The conformance suite under `protocol-tests/jsonrpc2` grows the
  streaming cases — open, events both directions, id echo, terminal
  result, terminal modeled error, each reserved code, clean close —
  authored as C++ suites pinning exact envelope text (the smithy
  `httpRequestTests` traits are request/response-shaped and cannot
  express a stream), normative per ADR-0002.
- ADR-0016's refusal diagnostic dies; its `:initial-request` seam is
  realized (by the first protocol to carry body-bound initial
  members) and its initial-response deferral is inherited verbatim,
  with `result: {}` as the placeholder that deferral leaves behind.
- The wire is text JSON-RPC end to end, so every existing JSON-RPC
  2.0 client library that can send notifications over a WebSocket can
  drive a stream without knowing smithy exists — the interop promise
  the protocol's identity makes, kept by the streams too.
