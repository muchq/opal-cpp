#include "smithy/http/server_dispatch.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "smithy/http/message.h"
#include "smithy/http/trace_context.h"
#include "smithy/http/transport.h"

namespace opal::http {
namespace {

HttpRequest SampleRequest() {
  HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  return request;
}

// Echoes the traceparent the handler chain observed, so tests can assert on
// the minted (or preserved) identity.
RequestHandler TraceEchoHandler() {
  return [](const HttpRequest& request) {
    HttpResponse response;
    response.headers.Set("x-seen", request.headers.Get("traceparent").value_or(""));
    return response;
  };
}

TEST(ServerDispatchTest, PassesThroughASuccessfulResponse) {
  RequestHandler handler = [](const HttpRequest&) {
    HttpResponse response;
    response.status = 201;
    response.body = "ok";
    return response;
  };
  const HttpResponse response = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_EQ(response.status, 201);
  EXPECT_EQ(response.body, "ok");
  EXPECT_FALSE(response.headers.Get("x-correlation-id").has_value());
}

TEST(ServerDispatchTest, StdExceptionBecomesA500WithCorrelationId) {
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse {
    throw std::out_of_range("boom");
  };
  const HttpResponse response = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_EQ(response.status, 500);
  const auto id = response.headers.Get("x-correlation-id");
  ASSERT_TRUE(id.has_value());
  EXPECT_FALSE(id->empty());
  // The body repeats the same id so a client report can be tied to the log line.
  EXPECT_NE(response.body.find(*id), std::string::npos);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
}

TEST(ServerDispatchTest, NonStdExceptionBecomesA500) {
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse { throw 42; };
  const HttpResponse response = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_EQ(response.status, 500);
  EXPECT_TRUE(response.headers.Get("x-correlation-id").has_value());
}

TEST(ServerDispatchTest, DistinctFailuresGetDistinctCorrelationIds) {
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse {
    throw std::runtime_error("x");
  };
  const auto a = InvokeHandlerGuarded(handler, SampleRequest());
  const auto b = InvokeHandlerGuarded(handler, SampleRequest());
  EXPECT_NE(a.headers.Get("x-correlation-id"), b.headers.Get("x-correlation-id"));
}

TEST(ServerDispatchTest, MintsATraceIdentityWhenTheClientSentNone) {
  // ADR-0011: every request entering the handler chain carries a parseable
  // traceparent — a fresh root when the client sent none — and each request
  // gets its own.
  const RequestHandler handler = TraceEchoHandler();
  const auto first = InvokeHandlerGuarded(handler, SampleRequest());
  const auto second = InvokeHandlerGuarded(handler, SampleRequest());
  const auto first_trace = ParseTraceparent(first.headers.Get("x-seen").value_or(""));
  const auto second_trace = ParseTraceparent(second.headers.Get("x-seen").value_or(""));
  ASSERT_TRUE(first_trace.has_value());
  ASSERT_TRUE(second_trace.has_value());
  EXPECT_NE(first_trace->trace_id, second_trace->trace_id);
}

TEST(ServerDispatchTest, KeepsAValidInboundTraceparentVerbatim) {
  const RequestHandler handler = TraceEchoHandler();
  HttpRequest request = SampleRequest();
  request.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  const auto response = InvokeHandlerGuarded(handler, request);
  EXPECT_EQ(response.headers.Get("x-seen"),
            "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
}

TEST(ServerDispatchTest, ReplacesAMalformedTraceparent) {
  // The W3C restart-the-trace rule: unparseable identity is not propagated.
  const RequestHandler handler = TraceEchoHandler();
  HttpRequest request = SampleRequest();
  request.headers.Set("traceparent", "not-a-traceparent");
  const auto response = InvokeHandlerGuarded(handler, request);
  const auto seen = response.headers.Get("x-seen").value_or("");
  EXPECT_NE(seen, "not-a-traceparent");
  EXPECT_TRUE(ParseTraceparent(seen).has_value()) << seen;
}

TEST(ServerDispatchTest, ReturnedServerErrorsGetTheTraceCorrelationId) {
  // A 500 the handler RETURNS correlates like one it throws: same header,
  // same identity. Sub-5xx responses are expected outcomes and stay clean.
  RequestHandler handler = [](const HttpRequest& request) {
    HttpResponse response;
    response.status =
        request.target == "/fail" ? 500 : (request.target == "/bad-gateway" ? 502 : 404);
    return response;
  };
  HttpRequest fail = SampleRequest();
  fail.target = "/fail";
  fail.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  EXPECT_EQ(InvokeHandlerGuarded(handler, fail).headers.Get("x-correlation-id").value_or(""),
            "0af7651916cd43dd8448eb211c80319c");

  HttpRequest gateway = SampleRequest();
  gateway.target = "/bad-gateway";  // the whole 5xx class stamps, not just 500
  EXPECT_EQ(
      InvokeHandlerGuarded(handler, gateway).headers.Get("x-correlation-id").value_or("").size(),
      32u);

  HttpRequest not_found = SampleRequest();
  not_found.target = "/missing";
  EXPECT_FALSE(
      InvokeHandlerGuarded(handler, not_found).headers.Get("x-correlation-id").has_value());
}

TEST(ServerDispatchTest, AHandlerSetCorrelationIdWins) {
  RequestHandler handler = [](const HttpRequest&) {
    HttpResponse response;
    response.status = 503;
    response.headers.Set("x-correlation-id", "app-chosen");
    return response;
  };
  EXPECT_EQ(
      InvokeHandlerGuarded(handler, SampleRequest()).headers.Get("x-correlation-id").value_or(""),
      "app-chosen");
}

TEST(ServerDispatchTest, DuplicatedTraceparentsRestartTheTrace) {
  // W3C counts multiple traceparent headers as malformed: the guard replaces
  // them with exactly one fresh root, so nothing downstream sees two.
  RequestHandler handler = [](const HttpRequest& request) {
    HttpResponse response;
    const auto all = request.headers.GetAll("traceparent");
    response.headers.Set("x-count", std::to_string(all.size()));
    response.headers.Set("x-seen", all.empty() ? "" : all.front());
    return response;
  };
  HttpRequest request = SampleRequest();
  request.headers.Add("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  request.headers.Add("traceparent", "00-11111111111111111111111111111111-2222222222222222-01");
  const auto response = InvokeHandlerGuarded(handler, request);
  EXPECT_EQ(response.headers.Get("x-count"), "1");
  const auto seen = ParseTraceparent(response.headers.Get("x-seen").value_or(""));
  ASSERT_TRUE(seen.has_value());
  EXPECT_NE(seen->trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_NE(seen->trace_id, "11111111111111111111111111111111");
}

TEST(ServerDispatchTest, RestartingTheTraceDropsTheStaleTracestate) {
  // Vendor state belongs to the trace it arrived with: a restart discards
  // it, while a continued trace keeps it.
  RequestHandler handler = [](const HttpRequest& request) {
    HttpResponse response;
    response.headers.Set("x-tracestate", request.headers.Get("tracestate").value_or("<none>"));
    return response;
  };
  HttpRequest restarted = SampleRequest();
  restarted.headers.Set("traceparent", "not-a-traceparent");
  restarted.headers.Set("tracestate", "vendor=stale");
  EXPECT_EQ(InvokeHandlerGuarded(handler, restarted).headers.Get("x-tracestate"), "<none>");

  HttpRequest continued = SampleRequest();
  continued.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  continued.headers.Set("tracestate", "vendor=live");
  EXPECT_EQ(InvokeHandlerGuarded(handler, continued).headers.Get("x-tracestate"), "vendor=live");
}

TEST(ServerDispatchTest, ExceptionCorrelationIdIsTheRequestsTraceId) {
  // One identity per request: the 500's correlation id is the trace id —
  // inbound when the client sent one, minted otherwise — so the clog line,
  // the client-visible body, and the distributed trace all agree.
  RequestHandler handler = [](const HttpRequest&) -> HttpResponse {
    throw std::runtime_error("boom");
  };
  HttpRequest request = SampleRequest();
  request.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  const auto continued = InvokeHandlerGuarded(handler, request);
  EXPECT_EQ(continued.headers.Get("x-correlation-id").value_or(""),
            "0af7651916cd43dd8448eb211c80319c");

  const auto minted = InvokeHandlerGuarded(handler, SampleRequest());
  const auto id = minted.headers.Get("x-correlation-id").value_or("");
  EXPECT_EQ(id.size(), 32u) << id;  // a 32-hex trace id, not a 36-char uuid
}

TEST(ServerDispatchTest, EmptyHandlerIsA503NotACrash) {
  const HttpResponse response = InvokeHandlerGuarded(RequestHandler{}, SampleRequest());
  EXPECT_EQ(response.status, 503);
  EXPECT_FALSE(response.headers.Get("x-correlation-id").has_value());
}

}  // namespace
}  // namespace opal::http
