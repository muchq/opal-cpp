# ADR-0010: Generated handlers receive the request context

**Status:** Accepted (2026-07-18)

## Context

Generated handler methods took only the typed input: `Op(const OpInput&)`. Everything the
model doesn't bind was invisible to the one place application logic runs — unmodeled headers,
the inbound `traceparent`, the client's address, the routing captures. Issue #46 tracks the
consequences: a handler cannot log or continue a trace, apply tenant/source policy, or open a
child span; the trace-id, peer-address, and correlation-id items — and #91's span work — all
gate on some form of request-metadata threading.

The transport boundary already had the right precedent: `HttpResponse::operation` is a
server-side annotation field that never touches the wire. And the generated route lambdas
already receive both the raw `HttpRequest` and the router's `RequestContext`
(labels + query params) — the metadata died exactly one call before the handler.

Three shapes were considered for the handler surface:

1. **A second parameter on every operation** — `Op(const OpInput&, const
   opal::server::RequestContext&)`, with `RequestContext` gaining the raw request.
2. **An opt-in overload pair** — keep the 1-arg pure virtual, add a defaulted 2-arg virtual
   delegating to it. Non-breaking, but a permanent two-method surface per operation and
   "which do I implement" ambiguity in the primary teaching artifacts.
3. **Ambient context** (thread-local accessor). Invisible coupling, hostile to the executor
   model (handlers may hop threads in future streaming work), rejected outright.

## Decision

Shape 1. Every generated handler method is
`Op(const OpInput& input, const opal::server::RequestContext& context)`:

- `RequestContext` (the type route lambdas already thread) gains
  `const http::HttpRequest* request`, set by `Router::Route` for the handler call's
  lifetime. Through it a handler reads unmodeled headers, the inbound `traceparent`
  (parse with `smithy/http/trace_context.h`), and the peer address.
- `HttpRequest` gains `peer_address` — an "ip:port" server-side annotation (the
  `HttpResponse::operation` precedent) stamped by both server transports; empty on the
  in-memory Loopback unless a test stamps its own.
- All three protocol generators thread the context to the call: simpleRestJson passes the
  route lambda's context, rpcv2Cbor names its previously discarded lambda parameter, and
  jsonRpc2 threads it through the per-operation `Handle<Op>` dispatch functions.
- The parameter is deliberately **not optional**: request metadata is as fundamental as the
  input (auth, tracing, and source policy all need it), and one signature keeps one way to
  do things. Handlers with no use for it leave the parameter unnamed.

This is a breaking change to the generated server surface, made while the project is
pre-release (docs/versioning.md): every in-repo implementer, the goldens, and the quickstart
mirror moved in the same change, and the CHANGELOG carries the Breaking callout for
commit-pinning consumers.

## Consequences

- Handlers can log/ban by source, read tenant/feature headers, and continue inbound traces —
  and the remaining #46 observability items (server-minted trace ids, correlation ids on
  returned-error 500s) plus #91's spans now have a handler-visible carrier to build on.
- Future per-request metadata follows one placement rule: whoever stamps a fact decides
  where it lives — transport-stamped facts become `HttpRequest` annotations (transports
  never see a `RequestContext`; `peer_address` is the precedent), framework/router-minted
  facts become `RequestContext` members — and `RequestContext` stays the single
  handler-facing access point either way, so the handler signature never changes again for
  metadata.
- Tests may construct `RequestContext{}` directly; `request` is null only in that
  hand-constructed case, and the field is documented accordingly.
