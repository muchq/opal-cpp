#ifndef SMITHY_SERVER_WEBSOCKET_ROUTER_H_
#define SMITHY_SERVER_WEBSOCKET_ROUTER_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "smithy/core/outcome.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/server/router.h"

namespace opal::server {

// A streaming operation's serve callback: the upgrade request, the routing
// context built from it (labels + query + the request pointer — exactly
// what Router::Route hands unary handlers), and the accepted session. Runs
// on the transport's handler thread and blocks for the session's lifetime;
// the WebSocket& is on_websocket's borrow, valid until the callback
// returns, whose return ends the session with a close handshake
// (beast_transport.h).
using StreamServe =
    std::function<void(const http::HttpRequest&, const RequestContext&, http::WebSocket&)>;

// A streaming operation's shared-session launch callback (ADR-0019): the
// same request and routing context as StreamServe, but the session arrives
// as an owner and the callback returns immediately — it is a launch point
// (typically starting a Detached coroutine over AsyncEventStream), not a
// serve loop. The session lives until a Close, the idle timeout, or the
// transport Stop()'s abort sweep, exactly as on_websocket_session
// documents (beast_transport.h).
using StreamServeSession = std::function<void(const http::HttpRequest&, const RequestContext&,
                                              std::shared_ptr<http::WebSocket>)>;

// Method + URI-pattern dispatch for WebSocket upgrades (ADR-0016): the
// unary Router's streaming parallel, sharing its pattern grammar and
// matching precedence (internal:: in router.h) so routing behavior cannot
// drift between the two. The upgrade path deliberately bypasses the HTTP
// middleware chain (ADR-0015), so a generated server's streaming routes
// mount in two lines:
//
//   options.websocket_gate = server.StreamRouter()->Gate();
//   options.on_websocket = server.StreamRouter()->Serve();
//
// The shared-session seam (ADR-0019) mounts a hand-built router — or the
// generated server's async constructor (ADR-0021) — the same way:
// AddSession routes, then:
//
//   options.websocket_gate = router.Gate();
//   options.on_websocket_session = router.ServeSession();
//
// One router serves ONE seam: the transport mounts at most one of
// on_websocket / on_websocket_session, so a route added for the other
// seam could never be dispatched — Add and AddSession therefore refuse to
// mix, failing loud at wiring time instead of deadening routes silently.
// (A process that wants both seams serves them from two transports — the
// transport itself mounts only one dispatcher — or moves everything onto
// one seam.) Gate() is seam-agnostic either way; the dispatchers close a
// wrong-seam upgrade with a log line naming the right mount instead of
// throwing on a transport thread.
//
// Application admission policy (auth, rate limits) composes by wrapping
// Gate(): run the application's refusal first, then defer to the router's.
//
//   options.websocket_gate =
//       [gate = router.Gate()](const http::HttpRequest& request)
//           -> std::optional<http::HttpResponse> {
//         if (!Authorized(request)) return MakeErrorResponse(401, "Unauthorized", "no token");
//         return gate(request);
//       };
class WebSocketRouter {
 public:
  // Fails on invalid patterns and on route conflicts (same method + pattern
  // shape), like Router::Add — and on mixing seams (see above). The
  // operation name is recorded for the observability wiring streaming
  // dispatch grows later; today it only documents the route.
  Outcome<Unit> Add(std::string_view method, std::string_view pattern, StreamServe serve,
                    std::string_view operation = "");

  // The shared-seam sibling of Add: same grammar, precedence, conflict and
  // seam rules; the winning route receives the session as an owner.
  Outcome<Unit> AddSession(std::string_view method, std::string_view pattern,
                           StreamServeSession serve, std::string_view operation = "");

  // The websocket_gate decision: nullopt admits an upgrade some route will
  // serve; refusals are shaped exactly like Router's dispatch failures —
  // 404 (no pattern match), 405 with an Allow header (pattern match, wrong
  // method), 400 (malformed target). The returned callable refers to this
  // router: keep the router alive, and complete Add/AddSession calls,
  // before the transport starts.
  std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)> Gate() const;

  // The on_websocket dispatcher: re-matches the upgrade request, builds the
  // RequestContext the way Router::Route does, and blocks in the winning
  // route's serve callback. A request no route matches — unreachable when
  // Gate() screened the upgrade — just closes the session. Same lifetime
  // rule as Gate().
  std::function<void(const http::HttpRequest&, http::WebSocket&)> Serve() const;

  // The on_websocket_session dispatcher: identical matching and context
  // construction, handing the winning AddSession route the owned session.
  // The no-match fallthrough closes the session the same way. Same
  // lifetime rule as Gate().
  std::function<void(const http::HttpRequest&, std::shared_ptr<http::WebSocket>)> ServeSession()
      const;

 private:
  struct StreamRoute {
    std::vector<internal::Segment> segments;
    // Exactly one is engaged, by the seam rule: serve for Add routes,
    // serve_session for AddSession routes.
    StreamServe serve;
    StreamServeSession serve_session;
    std::string operation;
  };

  enum class Seam { kNone, kBorrowed, kShared };

  // The shared tail of Add/AddSession: parse, conflict-check, commit the
  // filled route (its callback slot already engaged), latch the seam. The
  // public methods run their seam refusal first.
  Outcome<Unit> AddRoute(std::string_view method, std::string_view pattern, StreamRoute route,
                         Seam seam);

  // The best (most specific) matching route for the method, or nullptr.
  const StreamRoute* FindBest(const std::string& method,
                              const std::vector<std::string>& segments) const;

  // The dispatchers' shared core: normalize, find the best route, and on a
  // hit fill `context` exactly the way Router::Route does. Null on a miss
  // (malformed target or no matching route).
  const StreamRoute* MatchForServe(const http::HttpRequest& request, RequestContext& context) const;

  // Keyed by HTTP method, like Router's index: a request scans only its own
  // method's routes, and map order makes the 405 Allow list deterministic.
  std::map<std::string, std::vector<StreamRoute>> routes_;
  // Set by the first successful Add/AddSession; the other method then
  // refuses. A failed add latches nothing.
  Seam seam_ = Seam::kNone;
};

}  // namespace opal::server

#endif  // SMITHY_SERVER_WEBSOCKET_ROUTER_H_
