// Pins RequireOrigin (ADR-0018, issue #113): the allowlist match is scheme
// + host + port exact after normalization, absent Origin is admitted (this
// is cross-site-WebSocket-hijacking defense, not auth), and everything
// malformed or unlisted is a 403 — including "null" unless literally
// allowlisted.

#include "opal/server/origin_gate.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "opal/http/message.h"

namespace opal::server {
namespace {

http::HttpRequest UpgradeFrom(const std::string& origin) {
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/rooms/lobby/watch";
  request.headers.Set("origin", origin);
  return request;
}

void ExpectAdmitted(
    const std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)>& gate,
    const http::HttpRequest& request, const char* why) {
  EXPECT_FALSE(gate(request).has_value()) << why;
}

void ExpectRefused(
    const std::function<std::optional<http::HttpResponse>(const http::HttpRequest&)>& gate,
    const http::HttpRequest& request, const char* why) {
  const auto refusal = gate(request);
  ASSERT_TRUE(refusal.has_value()) << why;
  EXPECT_EQ(refusal->status, 403) << why;
}

TEST(OriginGateTest, ListedOriginsAreAdmitted) {
  const auto gate = RequireOrigin({"https://muchq.com", "http://localhost:8080"});
  ExpectAdmitted(gate, UpgradeFrom("https://muchq.com"), "exact match");
  ExpectAdmitted(gate, UpgradeFrom("http://localhost:8080"), "explicit port match");
}

TEST(OriginGateTest, UnlistedOriginsAre403) {
  const auto gate = RequireOrigin({"https://muchq.com"});
  ExpectRefused(gate, UpgradeFrom("https://evil.example"), "different host");
  ExpectRefused(gate, UpgradeFrom("http://muchq.com"), "different scheme");
  ExpectRefused(gate, UpgradeFrom("https://muchq.com:8443"), "different port");
  ExpectRefused(gate, UpgradeFrom("https://sub.muchq.com"), "subdomains are not the origin");
  ExpectRefused(gate, UpgradeFrom("https://muchq.com.evil.example"), "suffix spoof");
}

TEST(OriginGateTest, DefaultPortsCollapseTheWayRfc6454Requires) {
  // "https://a.com" and "https://a.com:443" are one origin — whichever
  // spelling the allowlist or the browser uses.
  const auto explicit_port = RequireOrigin({"https://muchq.com:443"});
  ExpectAdmitted(explicit_port, UpgradeFrom("https://muchq.com"), "default port in header");
  const auto implicit_port = RequireOrigin({"https://muchq.com"});
  ExpectAdmitted(implicit_port, UpgradeFrom("https://muchq.com:443"), "default port in list");
  ExpectRefused(implicit_port, UpgradeFrom("https://muchq.com:80"), "https on 80 is different");
}

TEST(OriginGateTest, MatchingIsAsciiCaseInsensitive) {
  const auto gate = RequireOrigin({"https://MuchQ.com"});
  ExpectAdmitted(gate, UpgradeFrom("HTTPS://muchq.COM"), "schemes and hosts are case-insensitive");
}

TEST(OriginGateTest, AnAbsentOriginIsAdmitted) {
  // Non-browser clients don't send Origin, and the attack this gate stops
  // cannot omit it — a native client sharing the endpoint keeps working.
  const auto gate = RequireOrigin({"https://muchq.com"});
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/rooms/lobby/watch";
  ExpectAdmitted(gate, request, "no origin header");
}

TEST(OriginGateTest, TheNullOriginNeedsAnExplicitListing) {
  const auto strict = RequireOrigin({"https://muchq.com"});
  ExpectRefused(strict, UpgradeFrom("null"), "null is refused by default");
  const auto sandboxed = RequireOrigin({"https://muchq.com", "null"});
  ExpectAdmitted(sandboxed, UpgradeFrom("null"), "null listed explicitly");
  ExpectAdmitted(sandboxed, UpgradeFrom("NULL"), "case-insensitive like the rest");
}

TEST(OriginGateTest, MalformedAndDuplicateOriginsAre403) {
  const auto gate = RequireOrigin({"https://muchq.com"});
  ExpectRefused(gate, UpgradeFrom(""), "empty origin");
  ExpectRefused(gate, UpgradeFrom("muchq.com"), "bare hostname");
  ExpectRefused(gate, UpgradeFrom("https://muchq.com/path"), "origins have no path");
  ExpectRefused(gate, UpgradeFrom("ftp://muchq.com"), "unsupported scheme");
  ExpectRefused(gate, UpgradeFrom("https://"), "missing host");
  ExpectRefused(gate, UpgradeFrom("https://muchq.com:notaport"), "bad port");
  // The classic parser-confusion shape: the allowed host as userinfo on a
  // hostile authority. Userinfo is a URL feature, not an origin one.
  ExpectRefused(gate, UpgradeFrom("https://muchq.com@evil.example"), "userinfo spoof");

  http::HttpRequest doubled = UpgradeFrom("https://muchq.com");
  doubled.headers.Add("origin", "https://muchq.com");
  ExpectRefused(gate, doubled, "duplicate origin headers");
}

TEST(OriginGateTest, ATrailingSlashOnAllowlistEntriesIsTolerated) {
  // The one shape that is clearly the same origin, not a path: the
  // copy-paste "https://muchq.com/" from a browser's address bar.
  const auto gate = RequireOrigin({"https://muchq.com/"});
  ExpectAdmitted(gate, UpgradeFrom("https://muchq.com"), "trailing slash entry");
}

TEST(OriginGateTest, RefusalIsTheStandardErrorShape) {
  const auto gate = RequireOrigin({"https://muchq.com"});
  const auto refusal = gate(UpgradeFrom("https://evil.example"));
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 403);
  EXPECT_EQ(refusal->headers.Get("content-type"), std::optional<std::string>("application/json"));
  EXPECT_NE(refusal->body.find("Forbidden"), std::string::npos);
}

TEST(OriginGateDeathTest, AMalformedAllowlistFailsFastAtConstruction) {
  // A typo'd entry would silently refuse every browser — that must not
  // survive first boot (ADR-0009), so construction dies with the entry.
  EXPECT_DEATH((void)RequireOrigin({"muchq.com"}), "RequireOrigin.*muchq.com");
  EXPECT_DEATH((void)RequireOrigin({"https://muchq.com/app"}), "RequireOrigin");
}

TEST(OriginGateTest, AnEmptyAllowlistRefusesEveryBrowser) {
  // Legal (a service mid-migration might admit only non-browser peers);
  // absent Origin still passes, every browser is refused.
  const auto gate = RequireOrigin({});
  ExpectRefused(gate, UpgradeFrom("https://muchq.com"), "empty allowlist");
  http::HttpRequest bare;
  ExpectAdmitted(gate, bare, "no origin header");
}

}  // namespace
}  // namespace opal::server
