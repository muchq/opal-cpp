#include <gtest/gtest.h>

#include "opal/http/headers.h"
#include "opal/http/loopback.h"
#include "opal/http/message.h"
#include "opal/http/uri.h"

namespace opal::http {
namespace {

TEST(HeadersTest, AcceptMatches) {
  using opal::http::AcceptMatches;
  EXPECT_TRUE(AcceptMatches("application/json", "application/json"));
  EXPECT_TRUE(AcceptMatches("Application/JSON", "application/json"));
  EXPECT_TRUE(AcceptMatches("*/*", "image/jpeg"));
  EXPECT_TRUE(AcceptMatches("image/*", "image/jpeg"));
  EXPECT_TRUE(AcceptMatches("text/html, application/json;q=0.9", "application/json"));
  EXPECT_FALSE(AcceptMatches("application/hal+json", "application/json"));
  EXPECT_FALSE(AcceptMatches("application/json", "image/jpeg"));
  EXPECT_FALSE(AcceptMatches("text/*", "image/jpeg"));
}

TEST(HeadersTest, HeaderNameStartsWith) {
  using opal::http::HeaderNameStartsWith;
  EXPECT_TRUE(HeaderNameStartsWith("x-foo-abc", "x-foo-"));
  EXPECT_TRUE(HeaderNameStartsWith("X-Foo-Abc", "x-foo-"));
  EXPECT_TRUE(HeaderNameStartsWith("anything", ""));
  EXPECT_FALSE(HeaderNameStartsWith("x-foo", "x-foo-"));
  EXPECT_FALSE(HeaderNameStartsWith("x-bar-abc", "x-foo-"));
}

TEST(HeadersTest, GetIsCaseInsensitive) {
  Headers headers;
  headers.Add("Content-Type", "application/json");
  EXPECT_EQ(headers.Get("content-type"), "application/json");
  EXPECT_EQ(headers.Get("CONTENT-TYPE"), "application/json");
  EXPECT_TRUE(headers.Has("Content-type"));
  EXPECT_FALSE(headers.Has("content-length"));
  EXPECT_EQ(headers.Get("missing"), std::nullopt);
}

TEST(HeadersTest, PreservesRepeatedValuesInOrder) {
  Headers headers;
  headers.Add("X-Tag", "a");
  headers.Add("x-tag", "b");
  EXPECT_EQ(headers.GetAll("X-TAG"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(headers.Get("x-tag"), "a");
  headers.Set("X-Tag", "only");
  EXPECT_EQ(headers.GetAll("x-tag"), (std::vector<std::string>{"only"}));
  headers.Remove("X-TAG");
  EXPECT_FALSE(headers.Has("x-tag"));
}

TEST(UriTest, EncodesPathSegmentsPerSmithySpec) {
  // Unreserved characters pass through; everything else is escaped.
  EXPECT_EQ(EncodePathSegment("azAZ09-_.~"), "azAZ09-_.~");
  EXPECT_EQ(EncodePathSegment("a b"), "a%20b");
  EXPECT_EQ(EncodePathSegment("a/b"), "a%2Fb");
  EXPECT_EQ(EncodePathSegment("a:b?c#d[e]f"), "a%3Ab%3Fc%23d%5Be%5Df");
  EXPECT_EQ(EncodePathSegment("café"), "caf%C3%A9");
  // Greedy labels keep '/' but escape everything else.
  EXPECT_EQ(EncodeGreedyPathSegment("a/b c/d"), "a/b%20c/d");
}

TEST(UriTest, PercentDecodeRoundTripsAndRejectsMalformed) {
  const auto decoded = PercentDecode("caf%C3%A9%20%2F%20bar");
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, "café / bar");
  EXPECT_FALSE(PercentDecode("%2").ok());
  EXPECT_FALSE(PercentDecode("%GG").ok());
  EXPECT_FALSE(PercentDecode("100%").ok());
}

TEST(UriTest, BuildsQueryStrings) {
  QueryString query;
  EXPECT_EQ(query.ToString(), "");
  query.Add("pageSize", "10");
  query.Add("filter", "a b&c");
  query.Add("empty", "");
  query.AddFlag("flag");
  EXPECT_EQ(query.ToString(), "?pageSize=10&filter=a%20b%26c&empty=&flag");
}

TEST(HeadersTest, SplitHeaderListValues) {
  EXPECT_EQ(SplitHeaderListValues("a, b,c"), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(SplitHeaderListValues("one"), (std::vector<std::string>{"one"}));
  EXPECT_EQ(SplitHeaderListValues("a, ,b"), (std::vector<std::string>{"a", "", "b"}));
  EXPECT_EQ(SplitHeaderListValues(""), (std::vector<std::string>{""}));
}

TEST(HeadersTest, MediaTypeOfStripsParametersAndCase) {
  EXPECT_EQ(MediaTypeOf("application/json"), "application/json");
  EXPECT_EQ(MediaTypeOf("Application/JSON; charset=utf-8"), "application/json");
  EXPECT_EQ(MediaTypeOf("  text/plain ; q=1"), "text/plain");
  EXPECT_EQ(MediaTypeOf(""), "");
}

TEST(HeadersTest, SplitHttpDateHeaderValues) {
  EXPECT_EQ(
      SplitHttpDateHeaderValues("Mon, 16 Dec 2019 23:48:18 GMT, Mon, 16 Dec 2019 23:48:18 GMT"),
      (std::vector<std::string>{"Mon, 16 Dec 2019 23:48:18 GMT", "Mon, 16 Dec 2019 23:48:18 GMT"}));
  EXPECT_EQ(SplitHttpDateHeaderValues("Mon, 16 Dec 2019 23:48:18 GMT"),
            (std::vector<std::string>{"Mon, 16 Dec 2019 23:48:18 GMT"}));
  EXPECT_TRUE(SplitHttpDateHeaderValues("").empty());
}

TEST(UriTest, QueryStringHasMatchesRawKeys) {
  QueryString query;
  EXPECT_FALSE(query.Has("a b"));
  query.Add("a b", "1");
  EXPECT_TRUE(query.Has("a b")) << "keys compare pre-encoding";
  EXPECT_FALSE(query.Has("a%20b"));
  EXPECT_FALSE(query.Has("other"));
}

TEST(UriTest, ParsesRequestTargets) {
  const auto target = ParseRequestTarget("/cities/a%20b/forecast?pageSize=10&flag&q=x%26y");
  ASSERT_TRUE(target.ok());
  EXPECT_EQ(target->path_segments, (std::vector<std::string>{"cities", "a b", "forecast"}));
  ASSERT_EQ(target->query_params.size(), 3u);
  EXPECT_EQ(target->query_params[0], (std::pair<std::string, std::string>{"pageSize", "10"}));
  EXPECT_EQ(target->query_params[1], (std::pair<std::string, std::string>{"flag", ""}));
  EXPECT_EQ(target->query_params[2], (std::pair<std::string, std::string>{"q", "x&y"}));
}

TEST(UriTest, ParsesRootAndTrailingSlashes) {
  const auto root = ParseRequestTarget("/");
  ASSERT_TRUE(root.ok());
  EXPECT_TRUE(root->path_segments.empty());
  const auto trailing = ParseRequestTarget("/a/");
  ASSERT_TRUE(trailing.ok());
  EXPECT_EQ(trailing->path_segments, (std::vector<std::string>{"a", ""}));
  EXPECT_FALSE(ParseRequestTarget("no-slash").ok());
  EXPECT_FALSE(ParseRequestTarget("/bad%2").ok());
}

TEST(UriTest, ParsesEndpoints) {
  const auto plain = ParseEndpoint("http://localhost");
  ASSERT_TRUE(plain.ok());
  EXPECT_EQ(plain->host, "localhost");
  EXPECT_EQ(plain->port, 80);
  EXPECT_EQ(plain->path_prefix, "");

  const auto full = ParseEndpoint("http://127.0.0.1:8080/api/v1/");
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full->host, "127.0.0.1");
  EXPECT_EQ(full->port, 8080);
  EXPECT_EQ(full->path_prefix, "/api/v1");

  const auto secure = ParseEndpoint("https://secure.example.com");
  ASSERT_TRUE(secure.ok());
  EXPECT_EQ(secure->scheme, "https");
  EXPECT_EQ(secure->port, 443);
  EXPECT_TRUE(secure->tls());
  EXPECT_FALSE(ParseEndpoint("ftp://example.com").ok());
  EXPECT_FALSE(ParseEndpoint("http://").ok());
  EXPECT_FALSE(ParseEndpoint("http://host:0").ok());
  EXPECT_FALSE(ParseEndpoint("http://host:notaport").ok());
  EXPECT_FALSE(ParseEndpoint("host:80").ok());
}

TEST(LoopbackTest, RoutesRequestsToHandler) {
  Loopback loopback;
  HttpRequest probe;
  probe.target = "/echo";
  EXPECT_FALSE(loopback.Send(probe).ok());  // no handler yet

  ASSERT_TRUE(loopback
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.status = 201;
                    response.headers.Set("x-echo-target", request.target);
                    response.body = request.body;
                    return response;
                  })
                  .ok());

  probe.method = "POST";
  probe.body = "ping";
  const auto response = loopback.Send(probe);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->status, 201);
  EXPECT_EQ(response->headers.Get("X-Echo-Target"), "/echo");
  EXPECT_EQ(response->body, "ping");

  loopback.Stop();
  EXPECT_FALSE(loopback.Send(probe).ok());
}

TEST(LoopbackTest, PeerAddressPassesThroughUnchanged) {
  // No connection, no stamping: the handler sees whatever the caller set —
  // empty by default, or a test-stamped address for peer-dependent handlers.
  Loopback loopback;
  ASSERT_TRUE(loopback
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.headers.Set(
                        "x-peer", request.peer_address.empty() ? "<empty>" : request.peer_address);
                    return response;
                  })
                  .ok());

  HttpRequest plain;
  EXPECT_EQ(loopback.Send(plain)->headers.Get("x-peer"), "<empty>");

  HttpRequest stamped;
  stamped.peer_address = "203.0.113.7:52814";
  EXPECT_EQ(loopback.Send(stamped)->headers.Get("x-peer"), "203.0.113.7:52814");
}

TEST(LoopbackTest, SendLeavesTheCallersRequestUnminted) {
  // Send promises the caller's request is untouched: the ingress mints on
  // the server-side copy (ADR-0011), never on the client's object.
  Loopback loopback;
  ASSERT_TRUE(loopback.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  HttpRequest request;
  ASSERT_TRUE(loopback.Send(request).ok());
  EXPECT_FALSE(request.headers.Has("traceparent"));
}

TEST(LoopbackTest, AsyncSendUsesSameHandler) {
  Loopback loopback;
  ASSERT_TRUE(loopback.Start([](const HttpRequest&) { return HttpResponse{204, {}, ""}; }).ok());
  HttpRequest request;
  auto future = loopback.SendAsync(request);
  const auto response = future.get();
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->status, 204);
}

TEST(HeadersTest, WireSafetyPredicatesRejectControlBytes) {
  // The outbound injection defense (issue #109): what may reach a field
  // line at all. Names are token-shaped...
  EXPECT_TRUE(ValidHeaderName("x-request-id"));
  EXPECT_TRUE(ValidHeaderName("X-Weird~Token!"));
  EXPECT_FALSE(ValidHeaderName(""));
  EXPECT_FALSE(ValidHeaderName("x-a\r\nevil"));
  EXPECT_FALSE(ValidHeaderName("x-a\n"));
  EXPECT_FALSE(ValidHeaderName("x a"));   // space corrupts the line
  EXPECT_FALSE(ValidHeaderName("x:a"));   // colon corrupts the boundary
  EXPECT_FALSE(ValidHeaderName("x\ta"));  // HTAB is a value privilege
  EXPECT_FALSE(ValidHeaderName(std::string_view("x\0y", 3)));
  EXPECT_FALSE(ValidHeaderName("x\x7f"));
  EXPECT_FALSE(ValidHeaderName("x\x0b"));  // VT — a control byte, not just CR/LF
  EXPECT_FALSE(ValidHeaderName("x\x0c"));  // FF

  // ...values admit HTAB and obs-text, never CR/LF/NUL/DEL.
  EXPECT_TRUE(ValidHeaderValue(""));
  EXPECT_TRUE(ValidHeaderValue("plain value with spaces"));
  EXPECT_TRUE(ValidHeaderValue("tab\tseparated"));
  EXPECT_TRUE(ValidHeaderValue("obs-text \xc3\xa9"));
  EXPECT_FALSE(ValidHeaderValue("split\r\nevil: y"));
  EXPECT_FALSE(ValidHeaderValue("bare\rcr"));
  EXPECT_FALSE(ValidHeaderValue("bare\nlf"));
  EXPECT_FALSE(ValidHeaderValue(std::string_view("nul\0", 4)));
  EXPECT_FALSE(ValidHeaderValue("del\x7f"));
  EXPECT_FALSE(ValidHeaderValue("vt\x0b"));  // VT/FF are controls, not just CR/LF
  EXPECT_FALSE(ValidHeaderValue("ff\x0c"));

  Headers headers;
  headers.Add("fine", "value");
  EXPECT_FALSE(FindUnsafeHeader(headers).has_value());
  headers.Add("location", "https://x/\r\nset-cookie: evil");
  ASSERT_TRUE(FindUnsafeHeader(headers).has_value());
  EXPECT_EQ(*FindUnsafeHeader(headers), "location");
}

TEST(HeadersTest, RequestLineFieldPredicateRejectsSpaceAndControls) {
  // The request-line sibling (issue #109): what may reach
  // "METHOD SP TARGET SP HTTP/1.1". Legitimate methods and percent-encoded
  // origin-form targets pass — including a colon, which (unlike a header
  // name) is a legal target character.
  EXPECT_TRUE(ValidRequestLineField("GET"));
  EXPECT_TRUE(ValidRequestLineField("/cities/a%20b?pageSize=10"));
  EXPECT_TRUE(ValidRequestLineField("/a:b/c;p=q?x=y&z"));   // colon/semicolon legal in a target
  EXPECT_TRUE(ValidRequestLineField("/path/caf\xc3\xa9"));  // raw UTF-8 obs bytes don't split
  EXPECT_TRUE(ValidRequestLineField(""));                   // empty: byte scan alone passes

  EXPECT_FALSE(ValidRequestLineField("/x HTTP/1.1"));               // space splits the line
  EXPECT_FALSE(ValidRequestLineField("/x\r\nEvil: 1"));             // CRLF injection
  EXPECT_FALSE(ValidRequestLineField("/x\revil"));                  // bare CR
  EXPECT_FALSE(ValidRequestLineField("/x\nevil"));                  // bare LF
  EXPECT_FALSE(ValidRequestLineField("GET\tPOST"));                 // HTAB
  EXPECT_FALSE(ValidRequestLineField(std::string_view("/\0", 2)));  // NUL
  EXPECT_FALSE(ValidRequestLineField("/x\x7f"));                    // DEL
  EXPECT_FALSE(ValidRequestLineField("/x\x0b"));                    // VT
  EXPECT_FALSE(ValidRequestLineField("/x\x0c"));                    // FF
}

}  // namespace
}  // namespace opal::http
