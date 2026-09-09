# ADR-0018: Negotiated JSON-text event-stream frames — the browser wire

**Status:** Accepted (2026-07-20). Phase 8 follow-on; issue #113.
Implemented: `opal::eventstream::EncodeJsonFrame`/`DecodeJsonFrame`
(`//runtime:eventstream_json`), subprotocol negotiation on
`BeastServerTransport` + `BeastWebSocketClient`, and
`opal::server::RequireOrigin`.

## Context

The Phase 8 wire is one binary event-stream frame (ADR-0014's CRC prelude +
typed header block + payload) per binary WebSocket message; text messages
are protocol errors that fail the session (ADR-0015). That framing is
specified, fuzz-tested, and cheap for native clients — and unreachable from
a browser: the `WebSocket` API gives a page text and binary messages and
nothing else, so consuming one event means hand-writing a binary codec in
JS against the spec. Issue #113 (a browser-served game hub assessing the
stack) asks for the wire a web page can speak with `JSON.parse` alone,
plus the two admission pieces every browser-facing endpoint rebuilds: an
auth path that works without upgrade-request headers (the browser
`WebSocket` constructor cannot set them) and an Origin allowlist.

ADR-0015 deliberately reserved the accept-decorator seam on the server
upgrade — "subprotocol negotiation / extra 101 headers … nothing can shape
the acceptance response" — without building it. This ADR builds it, for
exactly the purpose the reservation anticipated.

## Decisions

- **One negotiated alternate wire encoding, named by a subprotocol.** A
  client that offers `smithy.eventstream.v1+json` in
  `Sec-WebSocket-Protocol` on the upgrade — and only such a client — gets
  the token echoed in the 101 and a session whose frames are **text**
  WebSocket messages carrying a JSON envelope. Everyone else gets today's
  binary wire, headerless 101 included: native clients never negotiate,
  and a server with the mode off is byte-identical to one predating it.
  Negotiation is opt-in per transport
  (`BeastServerTransport::Options::websocket_accept_json_frames`), because
  only the application knows its services serve JSON payloads (see
  scoping below). `Start` refuses the flag without `on_websocket` — dead
  config, like a gate without a serve callback.
- **The envelope is a JSON object with exactly two members**:
  `{"event": "<member>", "payload": {...}}` for events, with
  `"exception"` in place of `"event"` for the error arm — the member-name
  discrimination of ADR-0016's `:event-type`/`:exception-type` headers,
  transposed. The type value is a non-empty string; `"payload"` is
  required and must be a JSON object (events are structures; the protocol
  serializes structures as objects). One event per text message — the
  one-message-one-event rule verbatim.
- **The translation is a transport detail.** The session facade still
  speaks `eventstream::Message`: on a JSON-mode session, `Send` renders
  the message's envelope (`:message-type`, `:event-type`/
  `:exception-type`) as the JSON envelope, and a received text frame
  parses back into exactly the `Message` the binary wire would have
  carried, `:content-type: application/json` stamped. `EventStream`, the
  routers, `SessionRegistry`, and every line of generated code are
  untouched — the closed-union semantics, terminal-exception rule, and
  `":initial-response"` reservation ride through unchanged because the
  layers that own them never learn the wire changed. The codec pair
  (`EncodeJsonFrame`/`DecodeJsonFrame`, `//runtime:eventstream_json`) is
  the one place the envelope member names live, the `json_frame.h` twin
  of envelope.h's rule that nothing else spells the header names.
- **Fail-closed transposes symmetrically.** In JSON mode, a *binary*
  message fails the session with a `protocol_error` close — the exact
  mirror of binary mode's posture on text. An envelope that is not an
  object, carries an unknown or duplicate-kind member, both or neither
  discriminator, a non-string or empty type, or a non-object payload is
  session-fatal the way a malformed binary frame is. Unknown *event
  types* keep their existing owner: the generated decoder rejects them
  terminally, same as binary. The size ceiling is `kMaxMessageBytes` on
  the text frame, both directions — the symmetric-bounds line again.
- **Scoped to JSON-payload services, enforced where the wire is minted.**
  `EncodeJsonFrame` refuses a message whose `:content-type` is present
  and not `application/json`, and one whose payload is not a JSON object
  — so an rpcv2Cbor event stream on a JSON-mode session fails on the
  first send instead of shipping CBOR bytes in a text frame. It also
  refuses messages carrying headers beyond the envelope's own: the JSON
  envelope has no header channel, and encode must refuse what the wire
  cannot represent (ADR-0014's rule, applied to this wire). When
  `@eventHeader` lands for the binary wire, this envelope needs a
  decision, not silence. Payload bytes are parsed and re-encoded through
  the runtime's JSON codec in both directions, so exactly one JSON
  dialect reaches generated serde regardless of what a browser typed.
- **The client can offer the mode, and falls back silently.**
  `BeastWebSocketClient::Options::offer_json_frames` adds the offer to
  the dial; an echo selects JSON framing, no echo means binary. Both
  modes carry the same `Message`s, so the fallback is invisible above
  the session — the knob exists for parity, tooling, and the negotiation
  tests, not because C++ clients should prefer JSON. A server that
  answers with a subprotocol the client never offered fails the dial.
  The generated client's `WebSocketDialRequest` does not grow the knob:
  generated clients speak binary, and a hand-wired dialer that wants the
  JSON wire can use `BeastWebSocketClient::Options` directly.
- **Origin checking becomes a composable gate.**
  `opal::server::RequireOrigin({"https://example.com", ...})` returns a
  `websocket_gate` that refuses (403) upgrades whose `Origin` is present
  and not on the allowlist — scheme + host + port exact after
  normalization (ASCII-lowercased scheme/host, default ports resolved).
  A request with *no* Origin header is admitted: non-browser clients
  don't send one, and the attack this gate stops — a hostile page
  driving a victim's browser, which always sends Origin — cannot omit
  it. `"null"` (sandboxed iframes, `file://`) is refused unless
  literally allowlisted. Malformed allowlist entries fail fast at
  construction (ADR-0009): a typo that would silently refuse every
  browser should not survive first boot. The gate composes by chaining,
  like every gate: run it, then defer to auth, then the router's.
- **The browser auth pattern is blessed in the production guide**, not in
  code: a short-lived, single-use ticket minted by an authenticated unary
  operation over HTTPS, carried as an `@httpQuery`-bound initial-request
  member, validated by the gate before the 101 — admission control stays
  ahead of the upgrade, and the query-string log exposure (every proxy
  logs targets) is bounded by the ticket's lifetime instead of a
  credential's. Cookies are the documented alternative where same-site
  topology allows; first-message auth is documented as *not* blessed
  (it moves auth past the gate). Smuggling tokens through
  `Sec-WebSocket-Protocol` is explicitly not sanctioned — that header is
  now a negotiation channel, and it is echoed and logged like any other.

## Alternatives rejected

- **A JS/TS codec package for the binary wire as the primary remedy**:
  issue #113's own secondary ask. It keeps the single-wire property but
  prices a `<script>` tag's entry at a few hundred lines of `DataView`
  work; the negotiated mode prices it at `JSON.parse`. The codec stays
  worth shipping for sophisticated web consumers — the byte-exact
  conformance vectors and fuzz corpus are the ready-made test bank — but
  it is additive and deferred until a consumer wants binary in the
  browser.
- **A parallel JSON-typed session API** (a `JsonEventStream` beside
  `EventStream`): would fork the generated surface and the router
  contract per wire mode; the transport-internal translation keeps one
  API and makes the mode invisible to every layer above the socket.
- **General subprotocol passthrough** (arbitrary tokens on
  Options, echoed blindly): a subprotocol names wire semantics; echoing
  a token the transport gives no meaning to manufactures interop
  promises nobody keeps. The seam stays; the one token the transport
  understands is the one it negotiates.
- **CBOR-in-the-browser / transport fallbacks (long-polling)**: issue
  #113's non-goals, adopted as this ADR's. WebSocket-only, JSON-only.
- **Requiring Origin on every upgrade** (refusing header-less requests):
  would break native clients sharing the endpoint and defends against
  nothing — a non-browser attacker sets any Origin it likes. The gate is
  cross-site-WebSocket-hijacking defense and composes with real auth.

## Consequences

- `//runtime:http_beast` grows a dependency on the new
  `//runtime:eventstream_json` (and thereby `//runtime:json` /
  nlohmann_json — already in every simpleRestJson consumer's graph;
  rpcv2Cbor-only consumers of the Beast transport now link the JSON
  codec as a passenger). `:eventstream` itself stays a leaf on `:core`.
- A browser client of a simpleRestJson streaming service is now
  `new WebSocket(url, "smithy.eventstream.v1+json")` plus
  `JSON.parse`/`JSON.stringify` — no codec, no build step. The
  production guide's event-stream section carries the worked pattern
  (ticket auth + `RequireOrigin` + the flag).
- Two wire encodings exist to test. The negotiation matrix, the
  fail-closed table for the JSON wire, and a browser-fidelity raw text
  client against the generated chat service are pinned in the runtime
  suite and the chat example; the in-memory pair and every layer above
  the socket remain single-wire and untouched.
- The `":initial-response"` reservation transposes: if initial-response
  support lands (ADR-0016's deferral), the JSON wire carries it as
  `{"event": ":initial-response", ...}` — the leading colon keeps it out
  of member space in both encodings.
- The close-code accessor seam ADR-0015 reserved stays reserved; this
  ADR consumed only the accept-decorator seam.
