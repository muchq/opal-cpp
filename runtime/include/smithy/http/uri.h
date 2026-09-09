#ifndef SMITHY_HTTP_URI_H_
#define SMITHY_HTTP_URI_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "smithy/core/outcome.h"

namespace opal::http {

// Percent-encodes for a single path segment (httpLabel): every byte outside
// RFC 3986 "unreserved" is escaped, including '/'.
std::string EncodePathSegment(std::string_view text);

// Percent-encodes for a greedy httpLabel: like EncodePathSegment but '/' is
// kept, since greedy labels span multiple segments.
std::string EncodeGreedyPathSegment(std::string_view text);

// Percent-encodes a query key or value (httpQuery).
std::string EncodeQueryComponent(std::string_view text);

// Decodes %XX escapes. Rejects malformed escapes.
Outcome<std::string> PercentDecode(std::string_view text);

// Accumulates query parameters into "?k=v&k2=v2" form with encoding applied.
class QueryString {
 public:
  // Adds "key=value" ("key=" when the value is empty).
  void Add(std::string_view key, std::string_view value);
  // Adds a bare valueless "key" (used for valueless @http query literals).
  void AddFlag(std::string_view key);
  // True when a parameter with this (raw, pre-encoding) key was added; used by
  // generated code for @httpQueryParams precedence (bound members win).
  bool Has(std::string_view key) const;
  // "" when empty, otherwise "?...".
  std::string ToString() const;

 private:
  struct Param {
    std::string key;
    std::string value;
    bool flag = false;
  };
  std::vector<Param> params_;
};

// Splits a raw target like "/cities/a%20b?page=2&size=10" into a decoded
// segment list and raw key/value pairs (values percent-decoded). Rejects
// malformed escapes.
struct RequestTarget {
  std::vector<std::string> path_segments;                         // decoded
  std::vector<std::pair<std::string, std::string>> query_params;  // decoded
};
Outcome<RequestTarget> ParseRequestTarget(std::string_view target);

// Minimal endpoint parser for "http(s)://host[:port][/prefix]". https
// endpoints need a TLS-capable transport (BeastHttpClient); the built-in
// socket transport is plaintext-only.
struct Endpoint {
  std::string scheme;  // "http" or "https"
  std::string host;
  int port = 80;            // https defaults to 443
  std::string path_prefix;  // no trailing '/', possibly empty

  bool tls() const { return scheme == "https"; }
};
Outcome<Endpoint> ParseEndpoint(std::string_view url);

}  // namespace opal::http

#endif  // SMITHY_HTTP_URI_H_
