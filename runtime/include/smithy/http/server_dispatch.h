#ifndef SMITHY_HTTP_SERVER_DISPATCH_H_
#define SMITHY_HTTP_SERVER_DISPATCH_H_

#include <sys/socket.h>

#include <string>

#include "smithy/http/message.h"
#include "smithy/http/transport.h"

namespace opal::http {

// Renders a peer's sockaddr as HttpRequest::peer_address ("ip:port", v6
// bracketed) — the one definition of the format, shared by every server
// transport so the stamps cannot drift. Empty when the address cannot be
// rendered.
std::string FormatPeerAddress(const sockaddr* address, socklen_t length);

// Invokes handler(request), converting any exception that escapes the handler
// into a 500 response instead of letting it propagate. Server transports call
// this rather than invoking the handler directly, so a throwing handler fails
// exactly one request instead of unwinding out of the transport's I/O thread
// and terminating the whole process.
//
// This is also where the server mints the request's trace identity
// (ADR-0011): a single valid inbound traceparent continues verbatim, while
// an absent, malformed, or duplicated one restarts the trace — a fresh root
// context replaces it in the request's headers, and any inbound tracestate
// is dropped with the abandoned trace — so Observe, the router, and
// handlers all see exactly one parseable identity on every request that
// enters the handler chain.
//
// The synthesized 500's "x-correlation-id" header (and the minimal JSON body
// repeating it) carries that trace id; the same id plus the exception's
// what() is written to std::clog, so an otherwise-silent crash leaves one
// greppable line tying the client-visible failure to the server-side cause
// and the distributed trace. Returned failures correlate the same way: any
// 5xx the handler chain produces without an x-correlation-id of its own is
// stamped with the trace id too (a handler-set id wins). handler may be
// empty — that yields a 503 (no correlation id: nothing ran).
HttpResponse InvokeHandlerGuarded(const RequestHandler& handler, HttpRequest request);

}  // namespace opal::http

#endif  // SMITHY_HTTP_SERVER_DISPATCH_H_
