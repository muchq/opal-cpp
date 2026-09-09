// Pins the structured access-log formatter (issue #203): the line's shape,
// its vocabulary's agreement with the metrics scrape, and — the part that
// needs care — that nothing a client puts in a request target can break the
// record or start a new one.
//
// The output is judged by nlohmann_json, a strict parser, because a
// collector is a strict parser: a line the formatter *thinks* is JSON but a
// parser rejects is a log entry that silently never arrives.

#include "smithy/server/access_log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "smithy/http/forwarded.h"
#include "smithy/http/message.h"
#include "smithy/server/metrics.h"
#include "smithy/server/middleware.h"

namespace opal::server {
namespace {

using nlohmann::json;
using Source = http::DerivedClient::Source;

RequestObservation Served() {
  RequestObservation o;
  o.method = "POST";
  o.target = "/tasks?verbose=1";
  o.operation = "AddTask";
  o.trace_parent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
  o.status = 201;
  o.duration = std::chrono::microseconds(1234);
  o.request_bytes = 19;
  o.response_bytes = 42;
  o.client.address = "203.0.113.7";
  o.client.source = Source::kForwarded;
  return o;
}

// The parser both decides validity and hands back the values. parse() would
// throw on a bad line; accept() first makes the failure an assertion with the
// offending text attached rather than an uncaught exception.
json Parse(const std::string& line) {
  EXPECT_TRUE(json::accept(line)) << "not strictly parseable JSON: " << line;
  return json::accept(line) ? json::parse(line) : json::object();
}

TEST(AccessLogTest, TheLineIsTheDocumentedKeysInTheDocumentedOrder) {
  // Literal on purpose: the key order and spelling are the contract a log
  // query pivots on, and a parser-level assertion would pass a reordering.
  EXPECT_EQ(FormatAccessLog(Served()),
            R"({"http_method":"POST","target":"/tasks?verbose=1","route":"AddTask",)"
            R"("status":201,"duration_us":1234,"request_bytes":19,"response_bytes":42,)"
            R"("client":"203.0.113.7","client_source":"forwarded","handler_threw":false,)"
            R"("trace_id":"4bf92f3577b34da6a3ce929d0e0e4736"})");
}

TEST(AccessLogTest, EveryKeyIsPresentOnAnEmptyObservation) {
  // A default observation is what a chain driven directly in tests
  // produces: no traceparent, no peer, no route. The schema does not shrink.
  const json parsed = Parse(FormatAccessLog(RequestObservation{}));
  const std::vector<std::string> keys = {
      "http_method",    "target", "route",         "status",        "duration_us", "request_bytes",
      "response_bytes", "client", "client_source", "handler_threw", "trace_id"};
  ASSERT_EQ(parsed.size(), keys.size());
  for (const auto& key : keys) {
    EXPECT_TRUE(parsed.contains(key)) << key;
  }
  EXPECT_EQ(parsed["route"], "unmatched");
  EXPECT_EQ(parsed["client"], "");
  EXPECT_EQ(parsed["client_source"], "unknown");
  EXPECT_EQ(parsed["trace_id"], "");
  EXPECT_EQ(parsed["status"], 0);
}

TEST(AccessLogTest, TheRouteSentinelIsTheOneTheScrapeUses) {
  // The whole reason the vocabulary lives in the runtime: `route` on a panel
  // and `route` on a line must be the same word. Both are produced from the
  // same observation here, and the log's spelling is asserted against the
  // scrape's rather than against a literal of its own.
  RequestObservation unrouted = Served();
  unrouted.operation = "";
  unrouted.status = 404;

  MetricsOptions options;
  options.enabled = true;
  options.service_name = "todo-service";
  MetricsRegistry registry(options);
  registry.Record(unrouted);
  const std::string scrape = registry.Expose();
  ASSERT_NE(scrape.find(R"(route="unmatched")"), std::string::npos) << scrape;

  const json parsed = Parse(FormatAccessLog(unrouted));
  const std::string route = parsed["route"];
  EXPECT_NE(scrape.find("route=\"" + route + "\""), std::string::npos)
      << "the line says route=" << route << " and the scrape does not";
  EXPECT_EQ(route, kUnmatchedRoute) << "both spell the shared sentinel";
}

TEST(AccessLogTest, TheMethodIsVerbatimWhereTheScrapeCollapsesIt) {
  // The scrape folds invented verbs into CUSTOM to bound cardinality. A log
  // line has no series to protect, and is where you find out what the verb
  // was — so this is the one deliberate divergence from the metric labels.
  RequestObservation o = Served();
  o.method = "PURGE";
  const json parsed = Parse(FormatAccessLog(o));
  EXPECT_EQ(parsed["http_method"], "PURGE");
}

TEST(AccessLogTest, ControlCharactersQuotesAndBackslashesInTheTargetAreEscaped) {
  RequestObservation o = Served();
  o.target = std::string("/a\"b\\c\n\r\t\b\f") + '\x01' + '\x1f' + "\x7f end";
  const std::string line = FormatAccessLog(o);
  // Short forms where JSON has them, \u00XX below 0x20 otherwise; 0x7F is
  // not a JSON control character and passes through.
  EXPECT_NE(line.find(R"("target":"/a\"b\\c\n\r\t\b\f\u0001\u001f)"
                      "\x7f end\""),
            std::string::npos)
      << line;
  const json parsed = Parse(line);
  EXPECT_EQ(parsed["target"], o.target) << "escaping must be reversible";
}

TEST(AccessLogTest, ACraftedTargetCannotTerminateTheRecordOrStartAnother) {
  // The log-injection shape: a URI that carries the closing quote, a fake
  // status, and a newline followed by a whole second record. Escaped, it is
  // one string value inside one object, and the fake fields are not fields.
  RequestObservation o = Served();
  o.target = "/x\",\"status\":200}\n{\"http_method\":\"GET\",\"status\":200";
  o.status = 500;
  const std::string line = FormatAccessLog(o);
  EXPECT_EQ(line.find('\n'), std::string::npos) << "a raw newline is a second record";
  const json parsed = Parse(line);
  EXPECT_EQ(parsed.size(), 11u) << "the injected keys became keys";
  EXPECT_EQ(parsed["status"], 500) << "the injected status won";
  EXPECT_EQ(parsed["target"], o.target);
}

TEST(AccessLogTest, ValidUtf8PassesThroughAndInvalidBytesAreReplaced) {
  RequestObservation o = Served();
  o.target = "/caf\xC3\xA9/\xE2\x9C\x93/\xF0\x9F\x98\x80";  // é ✓ 😀
  const json parsed = Parse(FormatAccessLog(o));
  EXPECT_EQ(parsed["target"], o.target) << "multi-byte sequences are not escaped byte-wise";

  // A stray continuation byte, a truncated 3-byte sequence, an overlong
  // encoding of '/', a surrogate, and 0xFF: each bad byte becomes one U+FFFD,
  // and the line stays acceptable to a strict parser — which is the point,
  // since the alternative is the collector dropping exactly the record about
  // the malformed request.
  o.target = "/\x80|\xE2\x9C|\xC0\xAF|\xED\xA0\x80|\xFF";
  const json again = Parse(FormatAccessLog(o));
  const std::string fffd = "\xEF\xBF\xBD";
  EXPECT_EQ(again["target"], "/" + fffd + "|" + fffd + fffd + "|" + fffd + fffd + "|" + fffd +
                                 fffd + fffd + "|" + fffd);
}

TEST(AccessLogTest, TheTraceIdIsParsedNotTheRawHeader) {
  RequestObservation o = Served();
  const json parsed = Parse(FormatAccessLog(o));
  EXPECT_EQ(parsed["trace_id"], "4bf92f3577b34da6a3ce929d0e0e4736");
  EXPECT_FALSE(parsed.contains("traceparent"));

  // Malformed is the same as absent: an empty id, not garbage under a key
  // that promises 32 hex digits.
  o.trace_parent = "not-a-traceparent";
  const json again = Parse(FormatAccessLog(o));
  EXPECT_EQ(again["trace_id"], "");
}

TEST(AccessLogTest, EveryClientSourceHasAName) {
  const std::vector<std::pair<Source, std::string>> expected = {
      {Source::kDirectPeer, "direct_peer"},
      {Source::kUntrustedHeaderIgnored, "untrusted_header_ignored"},
      {Source::kForwarded, "forwarded"},
      {Source::kTrustedTier, "trusted_tier"},
      {Source::kUnknown, "unknown"},
  };
  for (const auto& [source, name] : expected) {
    RequestObservation o = Served();
    o.client.source = source;
    const json parsed = Parse(FormatAccessLog(o));
    EXPECT_EQ(parsed["client_source"], name);
  }
}

TEST(AccessLogTest, AThrownHandlerReadsAsThrown) {
  RequestObservation o = Served();
  o.handler_threw = true;
  o.status = 500;
  o.response_bytes = 0;
  o.operation = "";
  const json parsed = Parse(FormatAccessLog(o));
  EXPECT_EQ(parsed["handler_threw"], true);
  EXPECT_EQ(parsed["status"], 500);
  EXPECT_EQ(parsed["response_bytes"], 0);
}

TEST(AccessLogTest, ExtraFieldsFollowTheBuiltInsInTheOrderGivenAndAreEscaped) {
  const std::string line = FormatAccessLog(
      Served(), {{"service_name", "todo-service"}, {"tenant", "acme \"inc\""}, {"z", "1"}});
  EXPECT_NE(line.find(R"("trace_id":"4bf92f3577b34da6a3ce929d0e0e4736","service_name":)"
                      R"("todo-service","tenant":"acme \"inc\"","z":"1"})"),
            std::string::npos)
      << line;
  const json parsed = Parse(line);
  EXPECT_EQ(parsed.size(), 14u);
  EXPECT_EQ(parsed["tenant"], "acme \"inc\"");
}

TEST(AccessLogDeathTest, AnExtraFieldThatShadowsABuiltInAborts) {
  // Two `status` keys in one object is ambiguous, and a collector that picks
  // one silently puts the wrong value under the right name. Keys are code
  // constants, so this is a contract violation (ADR-0009), not data.
  EXPECT_DEATH({ (void)FormatAccessLog(Served(), {{"status", "ok"}}); }, "shadows a built-in");
}

TEST(AccessLogDeathTest, ARepeatedExtraFieldAborts) {
  EXPECT_DEATH(
      { (void)FormatAccessLog(Served(), {{"tenant", "a"}, {"tenant", "b"}}); }, "given twice");
}

TEST(AccessLogDeathTest, AMalformedExtraFieldKeyAbortsRatherThanCollidingAfterReplacement) {
  // Uniqueness is only meaningful on what reaches the object. Both of these
  // keys are invalid UTF-8 and would each be replaced with two U+FFFD, so
  // without this check the line would carry one key twice and the repeat
  // check — comparing raw bytes — would never fire.
  EXPECT_DEATH(
      { (void)FormatAccessLog(Served(), {{"\xC0\xAF", "a"}, {"\x80\x81", "b"}}); },
      "not well-formed UTF-8");
  // A well-formed non-ASCII key is fine: it passes through byte-identical.
  const json parsed = Parse(FormatAccessLog(Served(), {{"caf\xC3\xA9", "x"}}));
  EXPECT_EQ(parsed["caf\xC3\xA9"], "x");
}

TEST(AccessLogDeathTest, AnEmptyExtraFieldKeyAborts) {
  EXPECT_DEATH({ (void)FormatAccessLog(Served(), {{"", "a"}}); }, "key is empty");
}

TEST(AccessLogTest, FormatsWhatObserveReports) {
  // The composition the header documents, end to end on a hand-driven chain:
  // Observe's sink formats the observation it is handed, and the line carries
  // the derived client — not the header — for the boundary Observe was given.
  auto trusted = http::TrustedProxies::Parse({"10.0.0.0/8"});
  ASSERT_TRUE(trusted.ok());
  std::vector<std::string> lines;
  auto handler = Chain({Observe(
                           [&lines](const RequestObservation& o) {
                             lines.push_back(FormatAccessLog(o, {{"service_name", "svc"}}));
                           },
                           nullptr, nullptr, *trusted)},
                       [](const http::HttpRequest&) {
                         http::HttpResponse response;
                         response.status = 200;
                         response.operation = "GetTask";
                         response.body = "{}";
                         return response;
                       });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/tasks/1";
  request.peer_address = "10.0.0.2:4242";
  request.headers.Set("x-forwarded-for", "203.0.113.9");
  request.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  (void)handler(request);

  ASSERT_EQ(lines.size(), 1u);
  const json parsed = Parse(lines[0]);
  EXPECT_EQ(parsed["route"], "GetTask");
  EXPECT_EQ(parsed["client"], "203.0.113.9");
  EXPECT_EQ(parsed["client_source"], "forwarded");
  EXPECT_EQ(parsed["response_bytes"], 2);
  EXPECT_EQ(parsed["trace_id"], "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ(parsed["service_name"], "svc");
}

}  // namespace
}  // namespace opal::server
