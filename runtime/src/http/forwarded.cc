#include "smithy/http/forwarded.h"

#include <arpa/inet.h>

#include <algorithm>
#include <optional>
#include <ranges>

#include "smithy/http/headers.h"

namespace opal::http {
namespace {

// A parsed numeric address: 4 (AF_INET) or 16 (AF_INET6) significant bytes.
// IPv4-mapped IPv6 is normalized to the embedded IPv4 on parse, so
// ::ffff:203.0.113.9 and 203.0.113.9 are one address everywhere — as a
// candidate, in an entry, or as a network base (ADR-0012).
struct Address {
  std::array<std::uint8_t, 16> bytes{};
  int family = 0;
};

constexpr std::array<std::uint8_t, 12> kV4MappedPrefix = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

// The leading-zero rule Apple's inet_pton does not enforce: it parses
// "010" as decimal 10 where glibc rejects the octal-ambiguous form.
// Structure and values stay inet_pton's job; this only pins the octets.
bool StrictDottedOctets(std::string_view dotted) {
  while (true) {
    const auto dot = dotted.find('.');
    const std::string_view octet = dotted.substr(0, dot);
    if (octet.empty() || octet.size() > 3 || (octet.size() > 1 && octet.front() == '0')) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    dotted.remove_prefix(dot + 1);
  }
}

std::optional<Address> ParseAddress(std::string_view text) {
  // Stack copy for inet_pton's terminator; an embedded NUL would silently
  // truncate inet_pton's view, so it is rejected outright.
  std::array<char, INET6_ADDRSTRLEN> terminated{};
  if (text.empty() || text.size() >= terminated.size() ||
      text.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  // The strict-reject contract is ours, not the platform's: Apple's libc
  // also accepts zone suffixes here (glibc never does), and zones are
  // handled only by ParseEndpoint's DropZone — reaching this point with a
  // '%' is malformed on every platform. Likewise the dotted-decimal tail
  // (a whole v4 address, or the embedded tail of one written inside v6)
  // must be octal-unambiguous.
  if (text.find('%') != std::string_view::npos) {
    return std::nullopt;
  }
  const auto last_colon = text.rfind(':');
  const std::string_view tail =
      last_colon == std::string_view::npos ? text : text.substr(last_colon + 1);
  if (tail.find('.') != std::string_view::npos && !StrictDottedOctets(tail)) {
    return std::nullopt;
  }
  std::copy(text.begin(), text.end(), terminated.begin());
  Address address;
  if (inet_pton(AF_INET, terminated.data(), address.bytes.data()) == 1) {
    address.family = AF_INET;
    return address;
  }
  if (inet_pton(AF_INET6, terminated.data(), address.bytes.data()) != 1) {
    return std::nullopt;
  }
  address.family = AF_INET6;
  if (std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), address.bytes.begin())) {
    std::copy(address.bytes.begin() + 12, address.bytes.end(), address.bytes.begin());
    std::fill(address.bytes.begin() + 4, address.bytes.end(), 0);
    address.family = AF_INET;
  }
  return address;
}

// Canonical numeric text for an address ParseAddress produced.
std::string FormatAddress(const Address& address) {
  std::array<char, INET6_ADDRSTRLEN> text{};
  if (inet_ntop(address.family, address.bytes.data(), text.data(), text.size()) == nullptr) {
    return {};
  }
  return text.data();
}

bool AllDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// RFC 4007 zone suffix ("%<nonempty zone>"), dropped from IPv6 text before
// parsing: getnameinfo stamps it for link-local peers ("fe80::1%eth0"), but
// the zone disambiguates link scope on this box — it is not client
// identity, and keeping it would collapse every link-local client onto the
// unparseable empty key. A bare trailing '%' is left for inet_pton to
// reject.
std::string_view DropZone(std::string_view text) {
  const auto percent = text.find('%');
  if (percent == std::string_view::npos || percent + 1 == text.size()) {
    return text;
  }
  return text.substr(0, percent);
}

// An address-with-optional-port endpoint reduced to the bare address —
// the forms x-forwarded-for elements and transport-stamped peer_addresses
// share: "v4", "v4:port", "[v6]", "[v6]:port", bare v6 (two or more
// colons). Expects trimmed input (header elements arrive via
// SplitHeaderListValues; peer_address is stamped unpadded). nullopt for
// anything else: "unknown", RFC 7239 obfuscated tokens, empty elements,
// garbage.
std::optional<Address> ParseEndpoint(std::string_view entry) {
  if (entry.empty()) {
    return std::nullopt;
  }
  if (entry.front() == '[') {
    const auto close = entry.find(']');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view port = entry.substr(close + 1);
    if (!port.empty() && (port.front() != ':' || !AllDigits(port.substr(1)))) {
      return std::nullopt;
    }
    return ParseAddress(DropZone(entry.substr(1, close - 1)));
  }
  const auto colons = std::count(entry.begin(), entry.end(), ':');
  if (colons == 1) {
    const auto colon = entry.find(':');
    if (!AllDigits(entry.substr(colon + 1))) {
      return std::nullopt;
    }
    return ParseAddress(entry.substr(0, colon));
  }
  return ParseAddress(colons == 0 ? entry : DropZone(entry));
}

bool PrefixMatch(const std::array<std::uint8_t, 16>& network,
                 const std::array<std::uint8_t, 16>& address, int prefix_bits) {
  const int full_bytes = prefix_bits / 8;
  if (!std::equal(network.begin(), network.begin() + full_bytes, address.begin())) {
    return false;
  }
  const int remainder = prefix_bits % 8;
  if (remainder == 0) {
    return true;
  }
  const auto mask = static_cast<std::uint8_t>(0xff << (8 - remainder));
  return (network[full_bytes] & mask) == (address[full_bytes] & mask);
}

}  // namespace

TrustedProxies TrustedProxies::None() { return TrustedProxies(); }

Outcome<TrustedProxies> TrustedProxies::Parse(const std::vector<std::string>& cidrs) {
  TrustedProxies result;
  result.networks_.reserve(cidrs.size());
  for (const std::string& cidr : cidrs) {
    const auto slash = cidr.find('/');
    const std::string_view base_text = std::string_view(cidr).substr(0, slash);
    const auto base = ParseAddress(base_text);
    if (!base.has_value()) {
      return Error::Validation("TrustedProxies: unparseable address in \"" + cidr + "\"");
    }
    const int max_bits = base->family == AF_INET ? 32 : 128;
    int prefix_bits = max_bits;
    if (slash != std::string::npos) {
      const std::string_view digits = std::string_view(cidr).substr(slash + 1);
      if (!AllDigits(digits) || digits.size() > 3) {
        return Error::Validation("TrustedProxies: malformed prefix in \"" + cidr + "\"");
      }
      prefix_bits = 0;
      for (const char c : digits) {
        prefix_bits = prefix_bits * 10 + (c - '0');
      }
      // A base written as IPv4-mapped IPv6 was normalized to the embedded
      // IPv4, so its prefix shifts across the /96 mapping range with it;
      // anything landing outside [0, max_bits] — including a mapped prefix
      // that would span non-IPv4 space — is a configuration error.
      if (base->family == AF_INET && base_text.find(':') != std::string_view::npos) {
        prefix_bits -= 96;
      }
      if (prefix_bits < 0 || prefix_bits > max_bits) {
        return Error::Validation("TrustedProxies: prefix out of range in \"" + cidr + "\"");
      }
    }
    result.networks_.push_back(Network{base->bytes, base->family, prefix_bits});
  }
  return result;
}

bool TrustedProxies::Contains(std::string_view address) const {
  const auto parsed = ParseAddress(address);
  return parsed.has_value() && ContainsBytes(parsed->bytes, parsed->family);
}

bool TrustedProxies::ContainsBytes(const std::array<std::uint8_t, 16>& bytes, int family) const {
  return std::any_of(networks_.begin(), networks_.end(), [&](const Network& network) {
    return network.family == family && PrefixMatch(network.bytes, bytes, network.prefix_bits);
  });
}

DerivedClient DeriveClient(const HttpRequest& request, const TrustedProxies& trusted) {
  const auto peer = ParseEndpoint(request.peer_address);
  if (!peer.has_value()) {
    return {};  // kUnknown via the Source default init
  }
  Address client = *peer;
  if (!trusted.ContainsBytes(client.bytes, client.family)) {
    return {FormatAddress(client), request.headers.Has("x-forwarded-for")
                                       ? DerivedClient::Source::kUntrustedHeaderIgnored
                                       : DerivedClient::Source::kDirectPeer};
  }
  // The walk half of the forwarded.h contract. client is assigned before
  // each trust test, so a malformed-entry stop and exhaustion both leave
  // the right answer — the last vetted hop or the leftmost all-trusted
  // entry — and either way the walk never left the trust set.
  std::vector<std::string> entries;
  for (const std::string& value : request.headers.GetAll("x-forwarded-for")) {
    for (std::string& element : SplitHeaderListValues(value)) {
      entries.push_back(std::move(element));
    }
  }
  for (const std::string& element : std::views::reverse(entries)) {
    const auto entry = ParseEndpoint(element);
    if (!entry.has_value()) {
      break;
    }
    client = *entry;
    if (!trusted.ContainsBytes(client.bytes, client.family)) {
      return {FormatAddress(client), DerivedClient::Source::kForwarded};
    }
  }
  return {FormatAddress(client), DerivedClient::Source::kTrustedTier};
}

std::string ClientAddress(const HttpRequest& request, const TrustedProxies& trusted) {
  return DeriveClient(request, trusted).address;
}

}  // namespace opal::http
