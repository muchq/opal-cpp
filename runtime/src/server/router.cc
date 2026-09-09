#include "smithy/server/router.h"

#include <algorithm>
#include <utility>

#include "smithy/http/uri.h"

namespace opal::server {

http::HttpResponse MakeErrorResponse(int status, std::string_view code, std::string_view message) {
  http::HttpResponse response;
  response.status = status;
  response.headers.Set("content-type", "application/json");
  // Both fields are JSON-string-safe for the inputs the runtime produces.
  response.body =
      "{\"code\":\"" + std::string(code) + "\",\"message\":\"" + std::string(message) + "\"}";
  return response;
}

namespace internal {

Outcome<std::vector<Segment>> ParsePattern(std::string_view pattern) {
  if (pattern.empty() || pattern[0] != '/') {
    return Error::Validation("router: pattern must start with '/': " + std::string(pattern));
  }
  std::vector<Segment> segments;
  std::string_view rest = pattern.substr(1);
  if (rest.empty()) return segments;  // "/" => zero segments
  while (true) {
    const auto slash = rest.find('/');
    const std::string_view raw = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    Segment segment;
    if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
      std::string_view name = raw.substr(1, raw.size() - 2);
      if (name.ends_with('+')) {
        segment.kind = Segment::Kind::kGreedy;
        name.remove_suffix(1);
      } else {
        segment.kind = Segment::Kind::kLabel;
      }
      if (name.empty()) {
        return Error::Validation("router: empty label in pattern: " + std::string(pattern));
      }
      segment.text = std::string(name);
    } else if (raw.empty()) {
      return Error::Validation("router: empty segment in pattern: " + std::string(pattern));
    } else {
      segment.kind = Segment::Kind::kLiteral;
      segment.text = std::string(raw);
    }
    const bool greedy = segment.kind == Segment::Kind::kGreedy;
    segments.push_back(std::move(segment));
    if (slash == std::string_view::npos) break;
    if (greedy) {
      return Error::Validation("router: greedy label must be the final segment: " +
                               std::string(pattern));
    }
    rest.remove_prefix(slash + 1);
  }
  return segments;
}

bool MatchSegments(const std::vector<Segment>& pattern, const std::vector<std::string>& segments,
                   PathLabels* labels) {
  if (labels != nullptr) labels->clear();
  const bool has_greedy = !pattern.empty() && pattern.back().kind == Segment::Kind::kGreedy;
  if (has_greedy) {
    if (segments.size() < pattern.size()) return false;
  } else if (segments.size() != pattern.size()) {
    return false;
  }
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const Segment& expected = pattern[i];
    switch (expected.kind) {
      case Segment::Kind::kLiteral:
        if (segments[i] != expected.text) return false;
        break;
      case Segment::Kind::kLabel:
        if (segments[i].empty()) return false;
        if (labels != nullptr) labels->emplace(expected.text, segments[i]);
        break;
      case Segment::Kind::kGreedy: {
        // The joined value ("seg/seg/...") is empty only when a lone empty
        // segment remains: two or more always meet at a '/'.
        if (segments.size() - i == 1 && segments[i].empty()) return false;
        if (labels != nullptr) {
          std::string joined;
          for (std::size_t j = i; j < segments.size(); ++j) {
            if (j > i) joined.push_back('/');
            joined += segments[j];
          }
          labels->emplace(expected.text, std::move(joined));
        }
        return true;
      }
    }
  }
  return true;
}

bool MoreSpecific(const std::vector<Segment>& a, const std::vector<Segment>& b) {
  // Segment-by-segment: literal beats label beats greedy. Longer patterns
  // rank higher when a prefix ties.
  const std::size_t common = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto rank = [](const Segment& s) {
      switch (s.kind) {
        case Segment::Kind::kLiteral:
          return 0;
        case Segment::Kind::kLabel:
          return 1;
        case Segment::Kind::kGreedy:
          return 2;
      }
      return 3;
    };
    if (rank(a[i]) != rank(b[i])) return rank(a[i]) < rank(b[i]);
  }
  return a.size() > b.size();
}

bool SameShape(const std::vector<Segment>& a, const std::vector<Segment>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].kind != b[i].kind) return false;
    if (a[i].kind == Segment::Kind::kLiteral && a[i].text != b[i].text) return false;
  }
  return true;
}

Outcome<http::RequestTarget> NormalizedTarget(const http::HttpRequest& request) {
  auto target = http::ParseRequestTarget(request.target);
  if (!target.ok()) return std::move(target).error();
  // Drop a trailing empty segment so "/a/" matches "/a". The parse is ours
  // to mutate — no per-request copy of the segment vector.
  std::vector<std::string>& segments = target->path_segments;
  if (!segments.empty() && segments.back().empty()) segments.pop_back();
  return target;
}

}  // namespace internal

Outcome<Unit> Router::Add(std::string_view method, std::string_view pattern, RouteHandler handler,
                          std::string_view operation) {
  auto segments = internal::ParsePattern(pattern);
  if (!segments) return std::move(segments).error();
  std::vector<RouteEntry>& bucket = routes_.try_emplace(std::string(method)).first->second;
  for (const RouteEntry& existing : bucket) {
    if (internal::SameShape(existing.segments, *segments)) {
      return Error::Validation("router: conflicting route: " + std::string(method) + " " +
                               std::string(pattern));
    }
  }
  bucket.push_back(RouteEntry{std::move(*segments), std::move(handler), std::string(operation)});
  return Unit{};
}

http::HttpResponse Router::Route(const http::HttpRequest& request) const {
  auto target = internal::NormalizedTarget(request);
  if (!target) {
    return MakeErrorResponse(400, "BadRequest", "malformed request target");
  }
  std::vector<std::string>& segments = target->path_segments;

  const RouteEntry* best = nullptr;
  if (const auto bucket = routes_.find(request.method); bucket != routes_.end()) {
    for (const RouteEntry& route : bucket->second) {
      if (!internal::MatchSegments(route.segments, segments, nullptr)) continue;
      if (best == nullptr || internal::MoreSpecific(route.segments, best->segments)) best = &route;
    }
  }
  if (best == nullptr) {
    // Miss path only: probe the other methods' buckets for the Allow list —
    // each contributes at most once, in map (deterministic) order.
    std::string allow;
    for (const auto& [method, bucket] : routes_) {
      if (method == request.method) continue;
      const bool any_match =
          std::any_of(bucket.begin(), bucket.end(), [&segments](const RouteEntry& route) {
            return internal::MatchSegments(route.segments, segments, nullptr);
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
  }
  RequestContext context;
  internal::MatchSegments(best->segments, segments, &context.labels);  // winner only
  context.query_params = std::move(target->query_params);
  context.request = &request;
  http::HttpResponse response = best->handler(request, context);
  if (!best->operation.empty()) response.operation = best->operation;
  return response;
}

}  // namespace opal::server
