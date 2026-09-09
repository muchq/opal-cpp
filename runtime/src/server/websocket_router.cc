#include "opal/server/websocket_router.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>

#include "opal/http/uri.h"

namespace opal::server {

namespace {

std::string RouteName(std::string_view method, std::string_view pattern) {
  return std::string(method) + " " + std::string(pattern);
}

}  // namespace

Outcome<Unit> WebSocketRouter::Add(std::string_view method, std::string_view pattern,
                                   StreamServe serve, std::string_view operation) {
  // The seam refusal outranks argument errors: mixing seams is the
  // categorical wiring mistake, reported whatever else is wrong.
  if (seam_ == Seam::kShared) {
    return Error::Validation("router: cannot Add " + RouteName(method, pattern) +
                             ": this router serves the shared seam "
                             "(AddSession/on_websocket_session); one router mounts one "
                             "dispatcher, so its routes serve one seam");
  }
  StreamRoute route;
  route.serve = std::move(serve);
  route.operation = std::string(operation);
  return AddRoute(method, pattern, std::move(route), Seam::kBorrowed);
}

Outcome<Unit> WebSocketRouter::AddSession(std::string_view method, std::string_view pattern,
                                          StreamServeSession serve, std::string_view operation) {
  if (seam_ == Seam::kBorrowed) {
    return Error::Validation("router: cannot AddSession " + RouteName(method, pattern) +
                             ": this router serves the borrowed seam (Add/on_websocket); one "
                             "router mounts one dispatcher, so its routes serve one seam");
  }
  StreamRoute route;
  route.serve_session = std::move(serve);
  route.operation = std::string(operation);
  return AddRoute(method, pattern, std::move(route), Seam::kShared);
}

Outcome<Unit> WebSocketRouter::AddRoute(std::string_view method, std::string_view pattern,
                                        StreamRoute route, Seam seam) {
  auto segments = internal::ParsePattern(pattern);
  if (!segments) return std::move(segments).error();
  std::vector<StreamRoute>& bucket = routes_.try_emplace(std::string(method)).first->second;
  for (const StreamRoute& existing : bucket) {
    if (internal::SameShape(existing.segments, *segments)) {
      return Error::Validation("router: conflicting route: " + RouteName(method, pattern));
    }
  }
  route.segments = std::move(*segments);
  bucket.push_back(std::move(route));
  seam_ = seam;  // only a successful add latches the seam
  return Unit{};
}

const WebSocketRouter::StreamRoute* WebSocketRouter::FindBest(
    const std::string& method, const std::vector<std::string>& segments) const {
  const auto bucket = routes_.find(method);
  if (bucket == routes_.end()) return nullptr;
  const StreamRoute* best = nullptr;
  for (const StreamRoute& route : bucket->second) {
    if (!internal::MatchSegments(route.segments, segments, nullptr)) continue;
    if (best == nullptr || internal::MoreSpecific(route.segments, best->segments)) best = &route;
  }
  return best;
}

std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)> WebSocketRouter::Gate()
    const {
  return [this](const http::HttpRequest& request) -> std::optional<http::HttpResponse> {
    auto target = internal::NormalizedTarget(request);
    if (!target.ok()) {
      return MakeErrorResponse(400, "BadRequest", "malformed request target");
    }
    if (FindBest(request.method, target->path_segments) != nullptr) {
      return std::nullopt;  // admitted: the mounted dispatcher serves it
    }
    // Miss path: probe the other methods' buckets for the Allow list, in
    // map (deterministic) order — Router::Route's shapes exactly.
    std::string allow;
    for (const auto& [method, bucket] : routes_) {
      if (method == request.method) continue;
      const bool any_match =
          std::any_of(bucket.begin(), bucket.end(), [&target](const StreamRoute& route) {
            return internal::MatchSegments(route.segments, target->path_segments, nullptr);
          });
      if (!any_match) continue;
      if (!allow.empty()) allow += ", ";
      allow += method;
    }
    if (!allow.empty()) {
      auto response = MakeErrorResponse(405, "MethodNotAllowed", "method not allowed");
      response.headers.Set("allow", allow);
      return response;
    }
    return MakeErrorResponse(404, "NotFound", "no route matches the request");
  };
}

const WebSocketRouter::StreamRoute* WebSocketRouter::MatchForServe(const http::HttpRequest& request,
                                                                   RequestContext& context) const {
  auto target = internal::NormalizedTarget(request);
  const StreamRoute* best = target.ok() ? FindBest(request.method, target->path_segments) : nullptr;
  if (best == nullptr) return nullptr;
  internal::MatchSegments(best->segments, target->path_segments, &context.labels);
  context.query_params = std::move(target->query_params);
  context.request = &request;
  return best;
}

std::function<void(const http::HttpRequest&, http::WebSocket&)> WebSocketRouter::Serve() const {
  // Say the wrong-seam mistake ONCE at wiring time, where the operator is
  // looking — an async-handler server mounted on on_websocket otherwise
  // fails one silent upgrade at a time (the per-upgrade line below).
  if (seam_ == Seam::kShared) {
    std::clog << "opal: router: every route here serves the shared seam (an async-handler "
                 "server); mount options.on_websocket_session = ServeSession(), not Serve()\n";
  }
  return [this](const http::HttpRequest& request, http::WebSocket& socket) {
    RequestContext context;
    const StreamRoute* best = MatchForServe(request, context);
    if (best == nullptr) {
      socket.Close();  // reachable only when Gate() did not screen the upgrade
      return;
    }
    if (!best->serve) {
      // The wrong dispatcher for this router's seam: an AddSession route
      // cannot be served borrowed. Never bad_function_call on a transport
      // thread — say what to mount, close, carry on.
      std::clog << "opal: router: route '" << best->operation
                << "' serves the shared seam; mount ServeSession(), not Serve()\n";
      socket.Close();
      return;
    }
    best->serve(request, context, socket);
  };
}

std::function<void(const http::HttpRequest&, std::shared_ptr<http::WebSocket>)>
WebSocketRouter::ServeSession() const {
  if (seam_ == Seam::kBorrowed) {
    std::clog << "opal: router: every route here serves the borrowed seam (a blocking-handler "
                 "server); mount options.on_websocket = Serve(), not ServeSession()\n";
  }
  return [this](const http::HttpRequest& request, std::shared_ptr<http::WebSocket> socket) {
    RequestContext context;
    const StreamRoute* best = MatchForServe(request, context);
    if (best == nullptr) {
      socket->Close();  // reachable only when Gate() did not screen the upgrade
      return;
    }
    if (!best->serve_session) {
      std::clog << "opal: router: route '" << best->operation
                << "' serves the borrowed seam; mount Serve(), not ServeSession()\n";
      socket->Close();
      return;
    }
    best->serve_session(request, context, std::move(socket));
  };
}

}  // namespace opal::server
