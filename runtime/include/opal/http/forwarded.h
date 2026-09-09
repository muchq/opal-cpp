#ifndef OPAL_HTTP_FORWARDED_H_
#define OPAL_HTTP_FORWARDED_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "opal/core/error.h"
#include "opal/core/outcome.h"
#include "opal/http/message.h"

namespace opal::http {

struct DerivedClient;

// The deployment's reverse-proxy trust boundary (ADR-0012): the set of
// addresses whose x-forwarded-for entries count during client-address
// derivation. Built from CIDR strings ("10.0.0.0/8", "2600:1f00::/24"); a
// bare address is a host route. Parse() returns an Error::Validation on a
// malformed entry — a misconfigured trust boundary must fail deployment, not
// silently widen or narrow, but the failure is recoverable config (like a bad
// bind address), so it is an Outcome, not an exception or an abort (ADR-0003).
// A copyable value; Contains is const and safe to share across threads.
//
// "Trust nothing" is a statement, not an absence (issue #104): None() is
// the greppable claim that the service is directly reachable — ClientAddress
// then reduces every request to its bare canonical peer. There is
// deliberately no public default constructor, because behind a proxy the
// accidental empty set (an unset config value) silently collapses all
// traffic onto the proxy's one policy key.
class TrustedProxies {
 public:
  static TrustedProxies None();

  // Parses CIDR and host-route entries into a trust boundary, or an
  // Error::Validation naming the first malformed entry.
  static Outcome<TrustedProxies> Parse(const std::vector<std::string>& cidrs);

  // Whether a bare numeric address (no port, no brackets) is inside any
  // trusted network. IPv4-mapped IPv6 matches as the embedded IPv4.
  // Unparseable input is in no network.
  bool Contains(std::string_view address) const;

 private:
  TrustedProxies() = default;

  friend DerivedClient DeriveClient(const HttpRequest& request, const TrustedProxies& trusted);

  // The parsed-form check DeriveClient's walk uses (one parse per entry,
  // not two): 4 (AF_INET) or 16 (AF_INET6) significant bytes.
  bool ContainsBytes(const std::array<std::uint8_t, 16>& bytes, int family) const;

  struct Network {
    std::array<std::uint8_t, 16> bytes{};
    int family = 0;
    int prefix_bits = 0;
  };
  std::vector<Network> networks_;
};

// The client's address as derived from the L4 peer and x-forwarded-for
// (ADR-0012), with its provenance (issue #104). address is in canonical
// numeric form (no port, no brackets, no v6 zone id; IPv4-mapped IPv6 as
// the embedded IPv4) — directly usable as a policy or metrics key. source
// records which path produced it — correct behavior on any single
// request; the distribution, not any single value, is the
// misconfiguration signal — docs/production-guide.md reads it.
struct DerivedClient {
  std::string address;
  enum class Source {
    kDirectPeer,              // untrusted peer, no x-forwarded-for: the client itself
    kUntrustedHeaderIgnored,  // untrusted peer; the header was present and ignored
    kForwarded,               // trusted walk ended on the client's entry
    kTrustedTier,             // the walk never left the trust set (no header,
                              // chain exhausted, or stopped by a malformed entry)
    kUnknown,                 // empty or unparseable peer (Loopback, handler
                              // chains driven directly in tests): address is ""
  };
  Source source = Source::kUnknown;
};

// The walk is anchored at request.peer_address, the one fact a client
// cannot forge: a peer outside the trust set IS the client and the header
// is ignored wholly. A trusted peer walks the entries right to left
// (rightmost were appended last, by the proxies closest to us), skipping
// trusted entries; the first untrusted entry is the client. Entries
// tolerate the forms real proxies emit — "ip", "ip:port", "[v6]",
// "[v6]:port", a zone id dropped — and multiple x-forwarded-for headers
// join in order (RFC 9110 list semantics). A chain exhausted with
// everything trusted yields the leftmost entry (the request originated
// inside the trusted tier); a malformed entry ("unknown", an obfuscated
// token, garbage) stops the walk at the last vetted position.
//
// Caveat, inherent to the recursive walk (nginx real_ip_recursive shares
// it): a client whose own address falls inside the trust set is skipped as
// a hop and can forge its ancestry — trust only networks that proxies
// alone occupy.
DerivedClient DeriveClient(const HttpRequest& request, const TrustedProxies& trusted);

// The simple form: DeriveClient's address alone.
std::string ClientAddress(const HttpRequest& request, const TrustedProxies& trusted);

}  // namespace opal::http

#endif  // OPAL_HTTP_FORWARDED_H_
