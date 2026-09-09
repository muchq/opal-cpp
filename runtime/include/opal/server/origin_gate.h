#ifndef OPAL_SERVER_ORIGIN_GATE_H_
#define OPAL_SERVER_ORIGIN_GATE_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "opal/http/message.h"

namespace opal::server {

// An Origin allowlist as a websocket_gate (ADR-0018, issue #113): the
// dozen lines every browser-facing WebSocket endpoint rebuilds, built
// once. Refuses (403) upgrades whose Origin header is present and not on
// the allowlist — scheme + host + port compared exactly after
// normalization (ASCII-lowercased, default ports resolved: "https://a.com"
// and "https://a.com:443" are the same origin; "http://a.com" is not).
//
// A request with NO Origin header is admitted: non-browser clients don't
// send one, and the attack this gate stops — a hostile page driving a
// victim's browser, which always sends Origin on WebSocket upgrades —
// cannot omit it. This is cross-site-WebSocket-hijacking defense, not
// authentication; compose it with an auth gate. The "null" origin
// (sandboxed iframes, file://) is refused unless "null" is literally
// allowlisted. Multiple or malformed Origin headers are refused.
//
// Allowlist entries are http(s) origins ("https://muchq.com",
// "http://localhost:8080") or the literal "null"; anything else — a path,
// a bare hostname, an unsupported scheme — fails fast at construction
// (ADR-0009): a typo that would silently refuse every browser must not
// survive first boot.
//
// Composes by chaining, like every gate — run it first, then defer:
//
//   options.websocket_gate =
//       [origin = opal::server::RequireOrigin({"https://muchq.com"}),
//        auth = MyAuthGate(), router = server.StreamRouter()->Gate()](
//           const opal::http::HttpRequest& request)
//           -> std::optional<opal::http::HttpResponse> {
//         if (auto refusal = origin(request)) return refusal;
//         if (auto refusal = auth(request)) return refusal;
//         return router(request);
//       };
std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)> RequireOrigin(
    const std::vector<std::string>& allowed_origins);

}  // namespace opal::server

#endif  // OPAL_SERVER_ORIGIN_GATE_H_
