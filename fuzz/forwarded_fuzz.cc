// Fuzz target: client-address derivation (ADR-0012). The walk parses two
// attacker-influenced inputs — the x-forwarded-for header and, indirectly,
// whatever ends up in peer_address — and must never crash on either.
// TrustedProxies::Parse must return a valid boundary or an Error::Validation,
// never anything else (in particular, never crash on arbitrary text).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "smithy/http/forwarded.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  static const opal::http::TrustedProxies trusted = *opal::http::TrustedProxies::Parse(
      {"10.0.0.0/8", "192.0.2.1", "2001:db8::/32", "::ffff:172.16.0.0/108"});

  // data[0] picks the peer so every run anchors a walk — trusted and
  // untrusted, v4/v6/mapped/empty — instead of waiting for the corpus to
  // discover an address inside the trust set. The rest is header text,
  // '\n'-split into multiple x-forwarded-for values (the GetAll join).
  static constexpr std::array<std::string_view, 6> kPeers = {
      "10.0.0.1:443", "203.0.113.9:52814", "[2001:db8::1]:443", "[::ffff:172.16.0.1]:1", "",
      "garbage"};
  opal::http::HttpRequest request;
  request.peer_address = std::string(kPeers[data[0] % kPeers.size()]);
  std::string_view rest(reinterpret_cast<const char*>(data + 1), size - 1);
  while (!rest.empty()) {
    const auto newline = rest.find('\n');
    request.headers.Add("x-forwarded-for", std::string(rest.substr(0, newline)));
    if (newline == std::string_view::npos) break;
    rest.remove_prefix(newline + 1);
  }

  // Source/address consistency invariants (issue #104): the label must
  // always agree with what the walk actually did.
  const auto derived = opal::http::DeriveClient(request, trusted);
  using Source = opal::http::DerivedClient::Source;
  if (opal::http::ClientAddress(request, trusted) != derived.address) std::abort();
  if ((derived.source == Source::kUnknown) != derived.address.empty()) std::abort();
  const bool header_present = request.headers.Has("x-forwarded-for");
  if (derived.source == Source::kDirectPeer && header_present) std::abort();
  if (derived.source == Source::kUntrustedHeaderIgnored && !header_present) std::abort();
  // The real trust invariant, as one biconditional: the walk never left
  // the trust set exactly when the derived address is itself trusted.
  if ((derived.source == Source::kTrustedTier) != trusted.Contains(derived.address)) {
    std::abort();
  }

  const std::string& client = derived.address;
  if (!client.empty()) {
    // Canonical output is a fixed point: a derived key is a valid
    // host-route trust entry (Parse must accept it), and re-deriving from it
    // changes nothing.
    const auto echo = opal::http::TrustedProxies::Parse({client});
    if (!echo.ok() || !echo->Contains(client)) std::abort();
    opal::http::HttpRequest again;
    again.peer_address = client;
    if (opal::http::ClientAddress(again, opal::http::TrustedProxies::None()) != client) {
      std::abort();
    }
  }

  const std::string_view text(reinterpret_cast<const char*>(data), size);
  // Contains is total on raw attacker text (unparseable is in no network),
  // and Parse returns a valid boundary or an Error — never a crash. When it
  // parses, exercise Contains on the result; the parser is fuzzed either way.
  (void)trusted.Contains(text);
  const auto probe = opal::http::TrustedProxies::Parse({std::string(text)});
  if (probe.ok()) {
    (void)probe->Contains("10.0.0.1");
  }
  return 0;
}
