#ifndef SMITHY_HTTP_HEADERS_H_
#define SMITHY_HTTP_HEADERS_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opal::http {

// HTTP header collection: case-insensitive names, repeated names preserved in
// insertion order (as list-valued headers require).
class Headers {
 public:
  // First value for the name, if present.
  std::optional<std::string> Get(std::string_view name) const;
  std::vector<std::string> GetAll(std::string_view name) const;
  bool Has(std::string_view name) const;

  // Replaces every existing value for the name.
  void Set(std::string_view name, std::string_view value);
  // Appends without touching existing values.
  void Add(std::string_view name, std::string_view value);
  void Remove(std::string_view name);

  const std::vector<std::pair<std::string, std::string>>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

// ASCII case-insensitive equality, as HTTP header names require.
bool HeaderNameEquals(std::string_view a, std::string_view b);

// True when an Accept header value (comma-separated media ranges; parameters
// ignored) accepts the given content type: exact match, "type/*", or "*/*".
bool AcceptMatches(std::string_view accept_header, std::string_view content_type);

// True when the header name starts with prefix, ASCII case-insensitively
// (@httpPrefixHeaders matching; an empty prefix matches every header).
bool HeaderNameStartsWith(std::string_view name, std::string_view prefix);

// Splits a comma-separated list-valued header into entries with surrounding
// whitespace trimmed ("a, b,c" -> {"a", "b", "c"}). Quoted-string entries are
// returned verbatim, quotes included — unescaping is the caller's concern.
std::vector<std::string> SplitHeaderListValues(std::string_view value);

// The media type of a Content-Type value: parameters and surrounding
// whitespace stripped, lowercased ("Application/JSON; charset=utf-8" ->
// "application/json").
std::string MediaTypeOf(std::string_view content_type);

// Splits a list-valued header of HTTP-dates, which themselves contain one
// comma ("Mon, 16 Dec 2019 23:48:18 GMT, Tue, 17 Dec ..."): consecutive
// comma-separated tokens are re-joined two at a time.
std::vector<std::string> SplitHttpDateHeaderValues(std::string_view value);

// The outbound header-injection defense (issue #109): whether a name or
// value may be written to an HTTP/1.1 wire at all. The line this draws is
// "cannot split or corrupt a field line", not full RFC 9110 token grammar:
// a name must be non-empty with no control bytes, space, colon, or DEL (an
// embedded CR/LF splits the message; space or colon corrupts the line), and
// a value may additionally contain HTAB (legal in field values). obs-text
// (>= 0x80) passes in BOTH — it is not strictly a token character in a name,
// but it cannot break framing, so it is not this defense's concern. The
// container above deliberately stores anything — inbound parsing reuses it —
// so the transports enforce these at their write paths, the same authority
// point that owns the framing headers.
bool ValidHeaderName(std::string_view name);
bool ValidHeaderValue(std::string_view value);

// First entry unsafe to serialize, if any: the name (or, for an unprintable
// name, its size-prefixed form is left to the caller's error text). Every
// outbound transport path checks this before writing.
std::optional<std::string> FindUnsafeHeader(const Headers& headers);

// The request-line sibling of the header defense (issue #109): whether a
// method or an origin-form target is safe to write into the client request
// line "METHOD SP TARGET SP HTTP/1.1 CRLF". Rejects space, every control
// byte (CR/LF split the line; the rest corrupt it), and DEL — but NOT
// colon, which is a legal target character (RFC 3986 pchar) unlike a header
// name. A method is an RFC 9110 token and a target is percent-encoded, so a
// conformant value never contains these; a raw one is request-line
// injection. Empty passes the byte scan — callers enforce non-emptiness
// where the grammar demands it (an empty method, not an empty target which
// the transports default to "/").
bool ValidRequestLineField(std::string_view field);

}  // namespace opal::http

#endif  // SMITHY_HTTP_HEADERS_H_
