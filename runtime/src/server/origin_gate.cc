#include "smithy/server/origin_gate.h"

#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "smithy/core/fatal.h"
#include "smithy/http/uri.h"
#include "smithy/server/router.h"

namespace opal::server {
namespace {

// The canonical form origins are compared in: lowercased scheme://host
// with the port always explicit, so "https://a.com" and
// "https://a.com:443" collide the way RFC 6454 says they must. nullopt
// for anything that is not an http(s) origin (or "null") — schemes,
// paths, userinfo, and garbage all land here.
std::optional<std::string> NormalizeOrigin(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lowered == "null") {
    return lowered;
  }
  const auto endpoint = http::ParseEndpoint(lowered);
  if (!endpoint.ok() || !endpoint->path_prefix.empty() ||
      endpoint->host.find('@') != std::string::npos) {
    // An origin is scheme://host[:port] and nothing else: a path or
    // userinfo means this is a URL, not an origin.
    return std::nullopt;
  }
  return endpoint->scheme + "://" + endpoint->host + ":" + std::to_string(endpoint->port);
}

}  // namespace

std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)> RequireOrigin(
    const std::vector<std::string>& allowed_origins) {
  std::set<std::string> allowed;
  for (const std::string& entry : allowed_origins) {
    auto normalized = NormalizeOrigin(entry);
    if (!normalized.has_value()) {
      // A malformed allowlist is a programming error that would silently
      // refuse every browser — fail fast at construction (ADR-0009).
      opal::internal::Fatal("RequireOrigin: not an http(s) origin or \"null\": " + entry);
    }
    allowed.insert(std::move(*normalized));
  }
  return [allowed = std::move(allowed)](
             const http::HttpRequest& request) -> std::optional<http::HttpResponse> {
    const std::vector<std::string> origins = request.headers.GetAll("origin");
    if (origins.empty()) {
      // No Origin means no browser: nothing for this gate to judge —
      // admission stays with the gates it composes with.
      return std::nullopt;
    }
    if (origins.size() == 1) {
      const auto normalized = NormalizeOrigin(origins.front());
      if (normalized.has_value() && allowed.contains(*normalized)) {
        return std::nullopt;
      }
    }
    return MakeErrorResponse(403, "Forbidden", "origin is not allowed");
  };
}

}  // namespace opal::server
