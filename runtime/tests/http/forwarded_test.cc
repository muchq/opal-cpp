// Pins ADR-0012: client-address derivation anchored at the L4 peer, with
// the rightmost-untrusted walk over x-forwarded-for. The suite's job is the
// security edges — every way an attacker-authored header must lose to the
// one fact a client cannot forge.

#include "smithy/http/forwarded.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace opal::http {
namespace {

// A valid trust boundary or a fatal test failure: the parse-error path is
// exercised explicitly by TrustedProxiesTest.MalformedConfigurationIsAnError.
TrustedProxies Trust(const std::vector<std::string>& cidrs) {
  auto parsed = TrustedProxies::Parse(cidrs);
  EXPECT_TRUE(parsed.ok()) << parsed.error().message();
  return parsed.ok() ? *std::move(parsed) : TrustedProxies::None();
}

HttpRequest RequestFrom(const std::string& peer, const std::vector<std::string>& xff = {}) {
  HttpRequest request;
  request.peer_address = peer;
  for (const std::string& value : xff) {
    request.headers.Add("x-forwarded-for", value);
  }
  return request;
}

TEST(TrustedProxiesTest, MatchesV4AndV6Cidrs) {
  const TrustedProxies trusted = Trust({"10.0.0.0/8", "2001:db8::/32"});
  EXPECT_TRUE(trusted.Contains("10.0.0.1"));
  EXPECT_TRUE(trusted.Contains("10.255.255.255"));
  EXPECT_FALSE(trusted.Contains("11.0.0.1"));
  EXPECT_TRUE(trusted.Contains("2001:db8::1"));
  EXPECT_TRUE(trusted.Contains("2001:db8:ffff::1"));
  EXPECT_FALSE(trusted.Contains("2001:db9::1"));
}

TEST(TrustedProxiesTest, MatchesMidBytePrefixBoundaries) {
  // /20 splits the third octet: 172.16.0.0–172.16.15.255.
  const TrustedProxies trusted = Trust({"172.16.0.0/20"});
  EXPECT_TRUE(trusted.Contains("172.16.15.255"));
  EXPECT_FALSE(trusted.Contains("172.16.16.0"));
  // /10 (remainder 2): a reversed mask shift passes /20's remainder-4
  // symmetry but fails here.
  const TrustedProxies cgnat = Trust({"100.64.0.0/10"});
  EXPECT_TRUE(cgnat.Contains("100.127.255.255"));
  EXPECT_FALSE(cgnat.Contains("100.128.0.1"));
}

TEST(TrustedProxiesTest, ABareAddressIsAHostRoute) {
  const TrustedProxies trusted = Trust({"192.0.2.7", "2001:db8::7"});
  EXPECT_TRUE(trusted.Contains("192.0.2.7"));
  EXPECT_FALSE(trusted.Contains("192.0.2.8"));
  EXPECT_TRUE(trusted.Contains("2001:db8::7"));
  EXPECT_FALSE(trusted.Contains("2001:db8::8"));
}

TEST(TrustedProxiesTest, AZeroPrefixTrustsTheWholeFamily) {
  const TrustedProxies v4_all = Trust({"0.0.0.0/0"});
  EXPECT_TRUE(v4_all.Contains("203.0.113.9"));
  EXPECT_FALSE(v4_all.Contains("2001:db8::1"));  // families never cross
  const TrustedProxies v6_all = Trust({"::/0"});
  EXPECT_TRUE(v6_all.Contains("2001:db8::1"));
  EXPECT_FALSE(v6_all.Contains("10.0.0.1"));
  // A mapped candidate is normalized to v4 before matching, so ::/0 is the
  // v6 family, not "everything".
  EXPECT_FALSE(v6_all.Contains("::ffff:10.0.0.1"));
}

TEST(TrustedProxiesTest, AHostBitsSetBaseMatchesAsIfMasked) {
  // No canonical-base requirement on config: matching masks to the prefix.
  const TrustedProxies trusted = Trust({"10.0.0.5/8"});
  EXPECT_TRUE(trusted.Contains("10.99.99.99"));
  EXPECT_FALSE(trusted.Contains("11.0.0.1"));
}

TEST(TrustedProxiesTest, RejectsAmbiguousNumericForms) {
  // The strict-reject contract is the runtime's, not the platform's:
  // Apple's inet_pton accepts leading-zero octets ("010" as decimal 10)
  // and zone suffixes where glibc rejects both, so ParseAddress pins them
  // down itself. No octal-ambiguous octets, no dotted shorthand, no zone
  // on a bare address (Contains takes bare numerics only) — identically on
  // every platform.
  const TrustedProxies trusted = Trust({"10.0.0.0/8", "fe80::/10"});
  EXPECT_FALSE(trusted.Contains("010.0.0.1"));
  EXPECT_FALSE(trusted.Contains("10.0.1"));
  EXPECT_FALSE(trusted.Contains("::ffff:010.0.0.1"));  // embedded tails too
  EXPECT_FALSE(trusted.Contains("fe80::1%eth0"));
  EXPECT_TRUE(trusted.Contains("fe80::1"));
}

TEST(TrustedProxiesTest, V4MappedV6IsTheEmbeddedV4Everywhere) {
  // A dual-stack listener reports ::ffff:10.0.0.1; a v4 CIDR must match it,
  // and a CIDR written in mapped form must match plain v4 (prefix
  // translated across the /96 mapping range).
  const TrustedProxies v4 = Trust({"10.0.0.0/8"});
  EXPECT_TRUE(v4.Contains("::ffff:10.0.0.1"));
  EXPECT_FALSE(v4.Contains("::ffff:11.0.0.1"));
  const TrustedProxies mapped = Trust({"::ffff:10.0.0.0/104"});
  EXPECT_TRUE(mapped.Contains("10.1.2.3"));
  EXPECT_TRUE(mapped.Contains("10.128.0.1"));  // /104 is exactly /8, not /9
  EXPECT_FALSE(mapped.Contains("11.0.0.1"));
  // The /96 boundary itself: the whole v4 family, still family-bound.
  const TrustedProxies whole = Trust({"::ffff:0.0.0.0/96"});
  EXPECT_TRUE(whole.Contains("203.0.113.9"));
  EXPECT_FALSE(whole.Contains("2001:db8::1"));
}

TEST(TrustedProxiesTest, V4CompatibleV6IsNotTheEmbeddedV4) {
  // Only the ::ffff:0:0/96 mapped form embeds v4. The deprecated
  // v4-compatible form (::10.0.0.1) stays IPv6: it must not inherit v4
  // trust — an attacker-writable entry would otherwise walk as trusted.
  const TrustedProxies v4 = Trust({"10.0.0.0/8"});
  EXPECT_FALSE(v4.Contains("::10.0.0.1"));
}

TEST(TrustedProxiesTest, NoneAndTheUnparseableTrustNothing) {
  // None() is the deliberate no-proxy topology (issue #104); there is no
  // default constructor to reach the empty set by accident. The derivation
  // pin is the issue's collapse scenario inverted: under None() even a
  // would-be proxy address is just the client, and its header is noise.
  const TrustedProxies nothing = TrustedProxies::None();
  EXPECT_FALSE(nothing.Contains("127.0.0.1"));
  EXPECT_FALSE(nothing.Contains("2001:db8::1"));
  const auto derived = DeriveClient(RequestFrom("10.0.0.1:443", {"203.0.113.9"}), nothing);
  EXPECT_EQ(derived.address, "10.0.0.1");
  EXPECT_EQ(derived.source, DerivedClient::Source::kUntrustedHeaderIgnored);
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  EXPECT_FALSE(trusted.Contains(""));
  EXPECT_FALSE(trusted.Contains("not-an-ip"));
  EXPECT_FALSE(trusted.Contains("10.0.0.1:443"));  // bare addresses only
}

TEST(TrustedProxiesTest, MalformedConfigurationIsAnError) {
  // A trust boundary that does not parse must fail deployment (ADR-0012), not
  // silently widen or narrow. It is recoverable config (like a bad bind
  // address), so Parse reports it as an Error::Validation — no exception
  // crosses the API and the runtime builds under -fno-exceptions (ADR-0003).
  for (const char* const bad :
       {"10.0.0.0/33", "2001:db8::/129", "10.0.0.0/", "10.0.0.0/8/8", "10.0.0.0/ 8", "gateway/8",
        "", "fe80::1%eth0/64",
        // A mapped base with a prefix spanning non-IPv4 space is
        // meaningless as a proxy trust boundary; both /96 neighbors pinned.
        "::ffff:10.0.0.0/64", "::ffff:10.0.0.0/95", "::ffff:10.0.0.0/129"}) {
    const auto parsed = TrustedProxies::Parse({bad});
    ASSERT_FALSE(parsed.ok()) << "expected a parse error for \"" << bad << "\"";
    EXPECT_EQ(parsed.error().kind(), ErrorKind::kValidation) << bad;
    EXPECT_NE(parsed.error().message().find("TrustedProxies:"), std::string::npos) << bad;
  }
  // One malformed entry fails the whole set, even alongside valid ones.
  EXPECT_FALSE(TrustedProxies::Parse({"10.0.0.0/8", "bogus/8"}).ok());
}

TEST(ClientAddressTest, AnUntrustedPeerIsTheClientAndTheHeaderIsIgnored) {
  // The direct-connect spoof: whatever the client wrote into
  // x-forwarded-for is noise when the TCP peer itself is not a proxy.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("203.0.113.9:52814", {"198.51.100.7, 10.0.0.1"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, ATrustedPeerYieldsTheRightmostUntrustedEntry) {
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"203.0.113.9"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, SpoofedPrefixEntriesNeverWin) {
  // The client sent its own x-forwarded-for; the edge proxy appended the
  // real address after it. The walk stops at the appended entry — the
  // attacker-authored prefix is unreachable.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"198.51.100.7, 203.0.113.9"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, AChainOfTrustedTiersSkipsToTheFirstUntrusted) {
  // LB -> internal proxy -> service: both tiers trusted, the entry before
  // them is the client.
  const TrustedProxies trusted = Trust({"10.0.0.0/8", "192.168.0.0/16"});
  const auto request = RequestFrom("10.0.0.1:443", {"198.51.100.7, 203.0.113.9, 192.168.1.1"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, AnExhaustedAllTrustedChainYieldsTheLeftmostEntry) {
  // The request originated inside the trusted tier itself.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"10.0.0.2, 10.0.0.3"});
  EXPECT_EQ(ClientAddress(request, trusted), "10.0.0.2");
}

TEST(ClientAddressTest, ATrustedPeerWithNoHeaderIsTheClient) {
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443"), trusted), "10.0.0.1");
}

TEST(ClientAddressTest, AMalformedEntryStopsTheWalkAtTheLastVettedPosition) {
  // Garbage is never trusted and never reached past: the derivation falls
  // back to the nearest vetted hop, not the junk and not what lies beyond.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto beyond = RequestFrom("10.0.0.1:443", {"203.0.113.9, unknown, 10.0.0.2"});
  EXPECT_EQ(ClientAddress(beyond, trusted), "10.0.0.2");
  const auto immediate = RequestFrom("10.0.0.1:443", {"not-an-ip"});
  EXPECT_EQ(ClientAddress(immediate, trusted), "10.0.0.1");
  const auto empty_element = RequestFrom("10.0.0.1:443", {"203.0.113.9,,10.0.0.2"});
  EXPECT_EQ(ClientAddress(empty_element, trusted), "10.0.0.2");
  // Leading-zero octets are octal-ambiguous, so they are garbage too.
  const auto octalish = RequestFrom("10.0.0.1:443", {"203.0.113.9, 010.0.0.2"});
  EXPECT_EQ(ClientAddress(octalish, trusted), "10.0.0.1");
  // A mid-walk stop never left the trust set, and its source says so.
  EXPECT_EQ(DeriveClient(beyond, trusted).source, DerivedClient::Source::kTrustedTier);
}

TEST(ClientAddressTest, TrustedHopsAreRecognizedInAnyEntryForm) {
  // The skip test parses, not string-matches: a trusted hop written with a
  // port or as IPv4-mapped IPv6 is still skipped, and a mapped client
  // derives as the embedded v4.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"203.0.113.9, 10.0.0.2:8080"}), trusted),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"203.0.113.9, ::ffff:10.0.0.2"}), trusted),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"::ffff:203.0.113.9"}), trusted),
            "203.0.113.9");
  // The v4-compatible form is NOT mapped: ::10.0.0.2 is an untrusted v6
  // client, not a trusted v4 hop.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"203.0.113.9, ::10.0.0.2"}), trusted),
            "::10.0.0.2");
}

TEST(ClientAddressTest, AClientInsideTheTrustSetCanForgeItsAncestry) {
  // The inherent recursive-walk caveat (ADR-0012, forwarded.h): a real
  // client whose own address falls inside the trust set is skipped as a
  // hop, so the attacker-authored entry before it wins. Trust only networks
  // that proxies alone occupy.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"6.6.6.6, 10.0.0.5"});
  EXPECT_EQ(ClientAddress(request, trusted), "6.6.6.6");
}

TEST(ClientAddressTest, EntriesTolerateTheFormsRealProxiesEmit) {
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  // Ports, whitespace, v6 brackets, bare v6 — each reduced to the bare
  // canonical address.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {" 203.0.113.9:52814 "}), trusted),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"[2001:db8::1]:443"}), trusted),
            "2001:db8::1");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"2001:DB8::1"}), trusted), "2001:db8::1");
  // A bracketed form with trailing junk is malformed, not half-parsed —
  // whether after the bracket or inside the port.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"[2001:db8::1]x"}), trusted), "10.0.0.1");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"[2001:db8::1]:abc"}), trusted), "10.0.0.1");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"203.0.113.9:port"}), trusted), "10.0.0.1");
  // The unspecified address parses like any other; whether "::" is an
  // acceptable policy key is the policy's call, not the derivation's.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"::"}), trusted), "::");
}

TEST(ClientAddressTest, ZoneIdsAreDroppedFromV6Endpoints) {
  // getnameinfo stamps the zone for link-local peers ("[fe80::1%eth0]:443");
  // the zone disambiguates link scope, not identity — link-local clients
  // key as the bare address instead of collapsing onto the empty key, and a
  // link-local proxy tier is trustable.
  const TrustedProxies v4 = Trust({"10.0.0.0/8"});
  EXPECT_EQ(ClientAddress(RequestFrom("[fe80::1%eth0]:443", {"203.0.113.9"}), v4), "fe80::1");
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"fe80::2%eth1"}), v4), "fe80::2");
  const TrustedProxies link = Trust({"fe80::/10"});
  EXPECT_EQ(ClientAddress(RequestFrom("[fe80::1%eth0]:443", {"203.0.113.9"}), link), "203.0.113.9");
  // A bare trailing '%' is not a zone; the entry is malformed.
  EXPECT_EQ(ClientAddress(RequestFrom("10.0.0.1:443", {"fe80::3%"}), v4), "10.0.0.1");
}

TEST(ClientAddressTest, MultipleHeadersJoinInOrder) {
  // Proxies may add a new x-forwarded-for header instead of appending to
  // the existing one; RFC 9110 list semantics make that equivalent.
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"198.51.100.7", "203.0.113.9, 10.0.0.2"});
  EXPECT_EQ(ClientAddress(request, trusted), "203.0.113.9");
  // The client in the FIRST header behind a spoofed prefix, the last header
  // all trusted hops: only a true rightmost-first walk across the joined
  // list finds it (a per-header-forward walk would answer 198.51.100.7).
  const auto crossing =
      RequestFrom("10.0.0.1:443", {"198.51.100.7, 203.0.113.9", "10.0.0.2, 10.0.0.3"});
  EXPECT_EQ(ClientAddress(crossing, trusted), "203.0.113.9");
}

TEST(ClientAddressTest, V6PeersAndMappedPeersDeriveBareCanonicalAddresses) {
  const TrustedProxies trusted = Trust({"2001:db8::/32"});
  // The transport stamps "[v6]:port"; the derivation strips to the bare
  // address, and a trusted v6 peer walks the header like any other.
  EXPECT_EQ(ClientAddress(RequestFrom("[2001:db8::1]:443", {"203.0.113.9"}), trusted),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("[2001:db9::1]:443", {"203.0.113.9"}), trusted),
            "2001:db9::1");
  // A dual-stack listener's mapped peer matches v4 trust and derives v4.
  const TrustedProxies v4 = Trust({"10.0.0.0/8"});
  EXPECT_EQ(ClientAddress(RequestFrom("[::ffff:10.0.0.1]:443", {"203.0.113.9"}), v4),
            "203.0.113.9");
  EXPECT_EQ(ClientAddress(RequestFrom("[::ffff:203.0.113.9]:52814"), v4), "203.0.113.9");
}

TEST(DeriveClientTest, TheSourceNamesTheDerivationPath) {
  // Issue #104: each source is correct on the single request; the
  // distribution is the misconfiguration signal.
  using Source = DerivedClient::Source;
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});

  const auto direct = DeriveClient(RequestFrom("203.0.113.9:52814"), trusted);
  EXPECT_EQ(direct.source, Source::kDirectPeer);
  EXPECT_EQ(direct.address, "203.0.113.9");

  const auto ignored = DeriveClient(RequestFrom("203.0.113.9:52814", {"198.51.100.7"}), trusted);
  EXPECT_EQ(ignored.source, Source::kUntrustedHeaderIgnored);
  EXPECT_EQ(ignored.address, "203.0.113.9");
  // Present-with-empty-value is still present-and-ignored.
  EXPECT_EQ(DeriveClient(RequestFrom("203.0.113.9:52814", {""}), trusted).source,
            Source::kUntrustedHeaderIgnored);

  const auto forwarded = DeriveClient(RequestFrom("10.0.0.1:443", {"203.0.113.9"}), trusted);
  EXPECT_EQ(forwarded.source, Source::kForwarded);
  EXPECT_EQ(forwarded.address, "203.0.113.9");

  // kTrustedTier covers all three walk-never-left-trust shapes: no header,
  // exhausted chain, malformed stop.
  EXPECT_EQ(DeriveClient(RequestFrom("10.0.0.1:443"), trusted).source, Source::kTrustedTier);
  EXPECT_EQ(DeriveClient(RequestFrom("10.0.0.1:443", {"10.0.0.2"}), trusted).source,
            Source::kTrustedTier);
  EXPECT_EQ(DeriveClient(RequestFrom("10.0.0.1:443", {"not-an-ip"}), trusted).source,
            Source::kTrustedTier);

  const auto unknown = DeriveClient(RequestFrom("", {"203.0.113.9"}), trusted);
  EXPECT_EQ(unknown.source, Source::kUnknown);
  EXPECT_EQ(unknown.address, "");
}

TEST(DeriveClientTest, ClientAddressIsTheAddressAlone) {
  const TrustedProxies trusted = Trust({"10.0.0.0/8"});
  const auto request = RequestFrom("10.0.0.1:443", {"203.0.113.9"});
  EXPECT_EQ(ClientAddress(request, trusted), DeriveClient(request, trusted).address);
}

TEST(ClientAddressTest, AnEmptyPeerDerivesEmpty) {
  // Loopback and handler chains driven directly in tests have no peer;
  // nothing is known, and empty never matches a trust set — so a spoofed
  // header cannot conjure an identity where the transport reported none.
  const TrustedProxies trusted = Trust({"10.0.0.0/8", "0.0.0.0/0"});
  EXPECT_EQ(ClientAddress(RequestFrom("", {"203.0.113.9"}), trusted), "");
  EXPECT_EQ(ClientAddress(RequestFrom("garbage-peer", {"203.0.113.9"}), trusted), "");
}

}  // namespace
}  // namespace opal::http
