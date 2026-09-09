#include "opal/http/uri.h"

#include <algorithm>
#include <cstdint>

namespace opal::http {
namespace {

constexpr std::string_view kHex = "0123456789ABCDEF";

bool IsUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '~';
}

std::string Encode(std::string_view text, bool keep_slash) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (IsUnreserved(c) || (keep_slash && c == '/')) {
      out.push_back(c);
    } else {
      const auto byte = static_cast<std::uint8_t>(c);
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0xF]);
    }
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

std::string EncodePathSegment(std::string_view text) { return Encode(text, /*keep_slash=*/false); }

std::string EncodeGreedyPathSegment(std::string_view text) {
  return Encode(text, /*keep_slash=*/true);
}

std::string EncodeQueryComponent(std::string_view text) {
  return Encode(text, /*keep_slash=*/false);
}

Outcome<std::string> PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    const int high = i + 1 < text.size() ? HexValue(text[i + 1]) : -1;
    const int low = i + 2 < text.size() ? HexValue(text[i + 2]) : -1;
    if (high < 0 || low < 0) {
      return Error::Serialization("uri: malformed percent escape");
    }
    out.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return out;
}

void QueryString::Add(std::string_view key, std::string_view value) {
  params_.push_back({EncodeQueryComponent(key), EncodeQueryComponent(value), false});
}

void QueryString::AddFlag(std::string_view key) {
  params_.push_back({EncodeQueryComponent(key), "", true});
}

bool QueryString::Has(std::string_view key) const {
  const std::string encoded = EncodeQueryComponent(key);
  return std::any_of(params_.begin(), params_.end(),
                     [&](const Param& param) { return param.key == encoded; });
}

std::string QueryString::ToString() const {
  if (params_.empty()) return "";
  std::string out = "?";
  for (std::size_t i = 0; i < params_.size(); ++i) {
    if (i > 0) out.push_back('&');
    out += params_[i].key;
    if (params_[i].flag) continue;
    // Always write '=': Smithy serializes empty query values as "key=".
    out.push_back('=');
    if (!params_[i].value.empty()) {
      out += params_[i].value;
    }
  }
  return out;
}

Outcome<RequestTarget> ParseRequestTarget(std::string_view target) {
  RequestTarget out;
  std::string_view path = target;
  std::string_view query;
  if (const auto question = target.find('?'); question != std::string_view::npos) {
    path = target.substr(0, question);
    query = target.substr(question + 1);
  }
  if (path.empty() || path[0] != '/') {
    return Error::Serialization("uri: request target must start with '/'");
  }
  path.remove_prefix(1);
  while (!path.empty()) {
    const auto slash = path.find('/');
    const std::string_view raw = slash == std::string_view::npos ? path : path.substr(0, slash);
    auto decoded = PercentDecode(raw);
    if (!decoded) return std::move(decoded).error();
    out.path_segments.push_back(std::move(*decoded));
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
    if (path.empty()) out.path_segments.emplace_back();  // trailing slash => empty segment
  }
  while (!query.empty()) {
    const auto amp = query.find('&');
    const std::string_view pair = amp == std::string_view::npos ? query : query.substr(0, amp);
    if (!pair.empty()) {
      const auto eq = pair.find('=');
      const std::string_view raw_key = eq == std::string_view::npos ? pair : pair.substr(0, eq);
      const std::string_view raw_value =
          eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
      auto key = PercentDecode(raw_key);
      if (!key) return std::move(key).error();
      auto value = PercentDecode(raw_value);
      if (!value) return std::move(value).error();
      out.query_params.emplace_back(std::move(*key), std::move(*value));
    }
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return out;
}

Outcome<Endpoint> ParseEndpoint(std::string_view url) {
  Endpoint endpoint;
  constexpr std::string_view kHttp = "http://";
  constexpr std::string_view kHttps = "https://";
  std::string_view rest;
  if (url.substr(0, kHttp.size()) == kHttp) {
    endpoint.scheme = "http";
    endpoint.port = 80;
    rest = url.substr(kHttp.size());
  } else if (url.substr(0, kHttps.size()) == kHttps) {
    endpoint.scheme = "https";
    endpoint.port = 443;
    rest = url.substr(kHttps.size());
  } else {
    return Error::Validation(
        "endpoint: expected an http:// or https:// URL (got: " + std::string(url) + ")");
  }
  const auto path_start = rest.find('/');
  std::string_view authority =
      path_start == std::string_view::npos ? rest : rest.substr(0, path_start);
  if (path_start != std::string_view::npos) {
    std::string_view prefix = rest.substr(path_start);
    while (prefix.ends_with('/')) prefix.remove_suffix(1);
    endpoint.path_prefix = std::string(prefix);
  }
  const auto colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    int port = 0;
    for (const char c : authority.substr(colon + 1)) {
      if (c < '0' || c > '9') return Error::Validation("endpoint: invalid port");
      port = port * 10 + (c - '0');
      if (port > 65535) return Error::Validation("endpoint: invalid port");
    }
    if (port == 0) return Error::Validation("endpoint: invalid port");
    endpoint.port = port;
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return Error::Validation("endpoint: missing host");
  endpoint.host = std::string(authority);
  return endpoint;
}

}  // namespace opal::http
