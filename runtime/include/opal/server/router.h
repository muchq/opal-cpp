#ifndef OPAL_SERVER_ROUTER_H_
#define OPAL_SERVER_ROUTER_H_

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "opal/core/outcome.h"
#include "opal/http/message.h"
#include "opal/http/uri.h"

namespace opal::server {

// Values captured from URI labels during route matching, keyed by label name.
// Greedy label values keep their embedded slashes (decoded).
using PathLabels = std::map<std::string, std::string, std::less<>>;

// Per-request context passed to matched handlers — and onward to generated
// handler methods, whose second parameter it is (ADR-0010). Beyond the
// routing captures it carries the raw request, so a handler can read what
// the typed input doesn't model: unmodeled headers, the inbound
// traceparent, the transport-stamped peer address (the how-to lives in
// docs/server-guide.md).
struct RequestContext {
  PathLabels labels;
  std::vector<std::pair<std::string, std::string>> query_params;  // decoded
  // The request being served; set by Router::Route for the lifetime of the
  // handler call, null only for hand-constructed contexts in tests.
  const http::HttpRequest* request = nullptr;
};

using RouteHandler =
    std::function<http::HttpResponse(const http::HttpRequest&, const RequestContext&)>;

namespace internal {

// The @http URI-pattern matcher, shared by Router and WebSocketRouter so
// pattern grammar and precedence cannot drift between unary and streaming
// routing (ADR-0016). Internal: generated code and applications route
// through the two routers, never through these.
struct Segment {
  enum class Kind { kLiteral, kLabel, kGreedy } kind = Kind::kLiteral;
  std::string text;  // literal text or label name
};

// Parses @http trait syntax ("/cities/{cityId}/forecast", greedy
// "/files/{path+}"). Fails on a missing leading '/', an empty segment or
// label name, and a greedy label before the final segment.
Outcome<std::vector<Segment>> ParsePattern(std::string_view pattern);

// True when `pattern` matches the request's decoded `segments`; the winning
// route's label values (greedy values re-joined with '/') land in `labels`.
// A null `labels` answers match/no-match without extracting label values.
bool MatchSegments(const std::vector<Segment>& pattern, const std::vector<std::string>& segments,
                   PathLabels* labels);

// True when `a` is more specific than `b` per the Smithy HTTP binding
// spec's precedence: segment by segment a literal outranks a label, a label
// outranks a greedy label; longer patterns rank higher when a prefix ties.
bool MoreSpecific(const std::vector<Segment>& a, const std::vector<Segment>& b);

// True when two patterns have the same shape (same length and kinds, same
// literal texts) and so would always match the same targets — the Add-time
// conflict test.
bool SameShape(const std::vector<Segment>& a, const std::vector<Segment>& b);

// The request-target normalization Router::Route and WebSocketRouter share
// (ADR-0016), so unary and streaming routing see the same segments: the
// decoded target with one trailing empty segment dropped ("/a/" matches
// "/a"). Fails on a malformed target (the routers' 400).
Outcome<http::RequestTarget> NormalizedTarget(const http::HttpRequest& request);

}  // namespace internal

// Method + URI-pattern dispatch per the Smithy HTTP binding spec.
//
// Patterns use @http trait syntax: "/cities/{cityId}/forecast" or greedy
// "/files/{path+}". Matching precedence follows the spec: at each segment a
// literal outranks a label, and a label outranks a greedy label; the most
// specific matching route wins regardless of registration order.
//
// Routes are indexed by method: a request scans only its own method's
// routes, label values are extracted once (for the winning route), and no
// candidate allocates during the scan. The miss path probes the other
// methods' buckets once to build the 405 Allow list — never more matching
// work than the old full scan (issue #46).
class Router {
 public:
  // Fails on invalid patterns and on route conflicts (same method + pattern
  // shape) — the generator surfaces this at build time. A non-empty
  // operation name is stamped onto HttpResponse::operation for
  // observability middleware.
  Outcome<Unit> Add(std::string_view method, std::string_view pattern, RouteHandler handler,
                    std::string_view operation = "");

  // Full dispatch: 404 (no pattern match), 405 (pattern match, wrong method,
  // with an Allow header), 400 (malformed target); otherwise the handler's
  // response.
  http::HttpResponse Route(const http::HttpRequest& request) const;

 private:
  struct RouteEntry {
    std::vector<internal::Segment> segments;
    RouteHandler handler;
    std::string operation;
  };

  // Keyed by HTTP method (exact match; methods are case-sensitive per
  // RFC 9110). Map order makes the 405 Allow list deterministic.
  std::map<std::string, std::vector<RouteEntry>> routes_;
};

// Uniform error response used by the router and by generated servers for
// framework-level failures (protocol-specific error bodies land in Phase 4).
http::HttpResponse MakeErrorResponse(int status, std::string_view code, std::string_view message);

// Raised (as a value) by generated input validation before user handlers run.
struct ValidationFailure {
  std::string path;  // member path, e.g. "input.cityId"
  std::string message;
};

}  // namespace opal::server

#endif  // OPAL_SERVER_ROUTER_H_
