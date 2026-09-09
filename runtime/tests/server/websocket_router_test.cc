// Pins ADR-0016's streaming router against the unary Router's behavior:
// the same pattern grammar, precedence, and context construction (drift
// between the two is the failure mode the shared matcher exists to
// prevent), Gate() refusals shaped like Router's dispatch failures, and
// Serve() dispatching the winning route's callback — plus the ADR-0019
// shared seam (issue #118): ServeSession's launch-point handoff, the
// seam-agnostic Gate, the one-seam mixing refusals, and the wrong-seam
// dispatcher degrading to a close instead of a throw.

#include "smithy/server/websocket_router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "smithy/http/websocket_pair.h"

namespace opal::server {
namespace {

http::HttpRequest Request(std::string method, std::string target) {
  http::HttpRequest request;
  request.method = std::move(method);
  request.target = std::move(target);
  return request;
}

// True when the peer observes the session's clean end within the deadline
// — a regression that drops a Close hangs a raw Receive to the bazel
// timeout; this fails legibly instead.
bool CleanlyClosedAt(const std::shared_ptr<http::WebSocket>& peer) {
  auto received = std::async(std::launch::async, [peer] { return peer->Receive(); });
  if (received.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    peer->Close();  // free the probe thread before failing
    return false;
  }
  auto outcome = received.get();
  return outcome.ok() && !outcome->has_value();
}

// What one dispatched serve call saw.
struct ServeRecord {
  std::string tag;
  RequestContext context;  // context.request compared by identity only
};

// A serve callback that records its dispatch into `record` (which may be
// null for routes a test never expects to fire).
StreamServe Recorder(std::shared_ptr<ServeRecord> record, const std::string& tag) {
  return [record = std::move(record), tag](const http::HttpRequest&, const RequestContext& context,
                                           http::WebSocket&) {
    if (record == nullptr) return;
    record->tag = tag;
    record->context = context;
  };
}

TEST(WebSocketRouterTest, ServeExtractsLabelsQueryAndRequestLikeRouter) {
  auto record = std::make_shared<ServeRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}/stream", Recorder(record, "chat"), "Chat").ok());

  const http::HttpRequest request = Request("GET", "/rooms/a%20b/stream?since=10&verbose");
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  router.Serve()(request, *socket);

  EXPECT_EQ(record->tag, "chat");
  ASSERT_EQ(record->context.labels.size(), 1U);
  EXPECT_EQ(record->context.labels.at("roomId"), "a b");  // labels arrive decoded
  const std::vector<std::pair<std::string, std::string>> expected_query = {{"since", "10"},
                                                                           {"verbose", ""}};
  EXPECT_EQ(record->context.query_params, expected_query);
  // The context's request IS the served request, Router::Route's wiring.
  EXPECT_EQ(record->context.request, &request);
}

TEST(WebSocketRouterTest, ServeDispatchesTheMostSpecificRoute) {
  auto literal = std::make_shared<ServeRecord>();
  auto label = std::make_shared<ServeRecord>();
  auto greedy = std::make_shared<ServeRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/streams/{name+}", Recorder(greedy, "greedy")).ok());
  ASSERT_TRUE(router.Add("GET", "/streams/{name}", Recorder(label, "label")).ok());
  ASSERT_TRUE(router.Add("GET", "/streams/live", Recorder(literal, "literal")).ok());
  const auto serve = router.Serve();

  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/streams/live"), *socket);
  EXPECT_EQ(literal->tag, "literal");  // literal outranks label outranks greedy
  serve(Request("GET", "/streams/weather"), *socket);
  EXPECT_EQ(label->tag, "label");
  EXPECT_EQ(label->context.labels.at("name"), "weather");
  serve(Request("GET", "/streams/us/west/alerts"), *socket);
  EXPECT_EQ(greedy->tag, "greedy");
  EXPECT_EQ(greedy->context.labels.at("name"), "us/west/alerts");  // slashes kept
}

TEST(WebSocketRouterTest, GateAdmitsMatchesAndRefusesLikeRouter) {
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}/stream", Recorder(nullptr, ""), "Chat").ok());
  ASSERT_TRUE(router.Add("PUT", "/rooms/{roomId}/stream", Recorder(nullptr, ""), "Push").ok());
  const auto gate = router.Gate();

  // A route will serve it: admitted.
  EXPECT_EQ(gate(Request("GET", "/rooms/lobby/stream")), std::nullopt);

  // Router's refusal shapes: 404 for a target no pattern serves...
  const auto not_found = gate(Request("GET", "/nowhere"));
  ASSERT_TRUE(not_found.has_value());
  EXPECT_EQ(not_found->status, 404);
  EXPECT_NE(not_found->body.find("NotFound"), std::string::npos);

  // ...405 with the deterministic Allow list for a method mismatch...
  const auto wrong_method = gate(Request("DELETE", "/rooms/lobby/stream"));
  ASSERT_TRUE(wrong_method.has_value());
  EXPECT_EQ(wrong_method->status, 405);
  EXPECT_EQ(wrong_method->headers.Get("allow").value_or(""), "GET, PUT");

  // ...and 400 for a malformed target.
  const auto malformed = gate(Request("GET", "/bad%2"));
  ASSERT_TRUE(malformed.has_value());
  EXPECT_EQ(malformed->status, 400);
}

TEST(WebSocketRouterTest, GateAndServeAgreeWithRouterOnTrailingSlashes) {
  auto record = std::make_shared<ServeRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms", Recorder(record, "rooms")).ok());
  EXPECT_EQ(router.Gate()(Request("GET", "/rooms/")), std::nullopt);  // "/a/" matches "/a"
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  router.Serve()(Request("GET", "/rooms/"), *socket);
  EXPECT_EQ(record->tag, "rooms");
}

TEST(WebSocketRouterTest, ServeClosesTheSessionWhenNothingMatches) {
  WebSocketRouter router;
  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}/stream", Recorder(nullptr, "")).ok());
  const auto serve = router.Serve();

  // A gate-bypassing request must not leave the session dangling: the
  // socket closes, so the peer sees the stream's clean end.
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/nowhere"), *socket);
  EXPECT_TRUE(CleanlyClosedAt(peer));

  // The malformed-target path closes the same way.
  auto [socket2, peer2] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/bad%2"), *socket2);
  EXPECT_TRUE(CleanlyClosedAt(peer2));
}

// The shared-seam recorder: SessionRecorder mirrors Recorder, and keeps
// the owned session so tests can prove it outlives the dispatch.
struct SessionRecord {
  std::string tag;
  RequestContext context;
  std::shared_ptr<http::WebSocket> session;
};

StreamServeSession SessionRecorder(std::shared_ptr<SessionRecord> record, const std::string& tag) {
  return [record = std::move(record), tag](const http::HttpRequest&, const RequestContext& context,
                                           std::shared_ptr<http::WebSocket> socket) {
    if (record == nullptr) return;
    record->tag = tag;
    record->context = context;
    record->session = std::move(socket);
  };
}

TEST(WebSocketRouterTest, ServeSessionExtractsLabelsQueryAndRequestAndHandsOverTheSession) {
  auto record = std::make_shared<SessionRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(
      router.AddSession("GET", "/rooms/{roomId}/stream", SessionRecorder(record, "chat"), "Chat")
          .ok());

  const http::HttpRequest request = Request("GET", "/rooms/a%20b/stream?since=10");
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  router.ServeSession()(request, socket);

  EXPECT_EQ(record->tag, "chat");
  ASSERT_EQ(record->context.labels.size(), 1U);
  EXPECT_EQ(record->context.labels.at("roomId"), "a b");  // labels arrive decoded
  const std::vector<std::pair<std::string, std::string>> expected_query = {{"since", "10"}};
  EXPECT_EQ(record->context.query_params, expected_query);
  EXPECT_EQ(record->context.request, &request);

  // Launch-point semantics: the callback returned, and the session it kept
  // is alive and usable — that is the whole point of the shared seam.
  ASSERT_NE(record->session, nullptr);
  ASSERT_TRUE(record->session->Send({.headers = {{":event-type", "late"}}}).ok());
  auto at_peer = peer->Receive();
  ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
}

TEST(WebSocketRouterTest, ServeSessionDispatchesTheMostSpecificRoute) {
  auto general = std::make_shared<SessionRecord>();
  auto specific = std::make_shared<SessionRecord>();
  auto greedy = std::make_shared<SessionRecord>();
  WebSocketRouter router;
  ASSERT_TRUE(router.AddSession("GET", "/rooms/{roomId}", SessionRecorder(general, "label")).ok());
  ASSERT_TRUE(router.AddSession("GET", "/rooms/lobby", SessionRecorder(specific, "literal")).ok());
  ASSERT_TRUE(router.AddSession("GET", "/rooms/{path+}", SessionRecorder(greedy, "greedy")).ok());
  const auto serve = router.ServeSession();

  const http::HttpRequest literal_request = Request("GET", "/rooms/lobby");
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  serve(literal_request, socket);
  EXPECT_EQ(specific->tag, "literal");
  EXPECT_TRUE(general->tag.empty());

  const http::HttpRequest label_request = Request("GET", "/rooms/attic");
  auto [socket2, peer2] = http::InMemoryWebSocketPair::Create();
  serve(label_request, socket2);
  EXPECT_EQ(general->tag, "label");

  // The greedy leg exercises the session path's own label extraction:
  // slashes kept, re-joined.
  const http::HttpRequest greedy_request = Request("GET", "/rooms/attic/dusty/corner");
  auto [socket3, peer3] = http::InMemoryWebSocketPair::Create();
  serve(greedy_request, socket3);
  EXPECT_EQ(greedy->tag, "greedy");
  EXPECT_EQ(greedy->context.labels.at("path"), "attic/dusty/corner");
}

TEST(WebSocketRouterTest, ServeSessionClosesTheSessionWhenNothingMatches) {
  WebSocketRouter router;
  ASSERT_TRUE(router.AddSession("GET", "/rooms/{roomId}", SessionRecorder(nullptr, "")).ok());
  const auto serve = router.ServeSession();

  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/nowhere"), socket);
  EXPECT_TRUE(CleanlyClosedAt(peer));

  auto [socket2, peer2] = http::InMemoryWebSocketPair::Create();
  serve(Request("GET", "/bad%2"), socket2);
  EXPECT_TRUE(CleanlyClosedAt(peer2));
}

TEST(WebSocketRouterTest, GateSpeaksForSessionRoutesToo) {
  WebSocketRouter router;
  ASSERT_TRUE(router.AddSession("GET", "/rooms/{roomId}", SessionRecorder(nullptr, "")).ok());
  ASSERT_TRUE(router.AddSession("PUT", "/rooms/{roomId}", SessionRecorder(nullptr, "")).ok());
  const auto gate = router.Gate();

  EXPECT_EQ(gate(Request("GET", "/rooms/lobby")), std::nullopt);  // admitted
  auto miss = gate(Request("GET", "/nowhere"));
  ASSERT_TRUE(miss.has_value());
  EXPECT_EQ(miss->status, 404);
  // Session routes span methods and feed the deterministic Allow list.
  auto wrong_method = gate(Request("DELETE", "/rooms/lobby"));
  ASSERT_TRUE(wrong_method.has_value());
  EXPECT_EQ(wrong_method->status, 405);
  EXPECT_EQ(wrong_method->headers.Get("allow").value_or(""), "GET, PUT");
}

TEST(WebSocketRouterTest, WrongSeamDispatcherClosesInsteadOfThrowing) {
  // The seam rule guards Add calls; mounting the wrong dispatcher must
  // degrade to the no-match close (with a log line naming the fix), never
  // to bad_function_call on the transport's threads.
  WebSocketRouter shared;
  ASSERT_TRUE(shared.AddSession("GET", "/a", SessionRecorder(nullptr, "")).ok());
  auto [socket, peer] = http::InMemoryWebSocketPair::Create();
  shared.Serve()(Request("GET", "/a"), *socket);  // the WRONG dispatcher
  EXPECT_TRUE(CleanlyClosedAt(peer));

  WebSocketRouter borrowed;
  ASSERT_TRUE(borrowed.Add("GET", "/a", Recorder(nullptr, "")).ok());
  auto [socket2, peer2] = http::InMemoryWebSocketPair::Create();
  borrowed.ServeSession()(Request("GET", "/a"), socket2);  // mirror direction
  EXPECT_TRUE(CleanlyClosedAt(peer2));
}

TEST(WebSocketRouterTest, SeamsRefuseToMix) {
  // The transport mounts at most one dispatcher, so mixed routes could
  // never all be served — the router fails loud at wiring time instead.
  WebSocketRouter borrowed_first;
  ASSERT_TRUE(borrowed_first.Add("GET", "/a", Recorder(nullptr, "")).ok());
  const auto mixed_in = borrowed_first.AddSession("GET", "/b", SessionRecorder(nullptr, ""));
  ASSERT_FALSE(mixed_in.ok());
  EXPECT_EQ(mixed_in.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(mixed_in.error().message().find("one seam"), std::string::npos);

  WebSocketRouter shared_first;
  ASSERT_TRUE(shared_first.AddSession("GET", "/a", SessionRecorder(nullptr, "")).ok());
  const auto mixed_back = shared_first.Add("GET", "/b", Recorder(nullptr, ""));
  ASSERT_FALSE(mixed_back.ok());
  EXPECT_EQ(mixed_back.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(mixed_back.error().message().find("one seam"), std::string::npos);

  // The seam is router-level, not per method bucket: mixing across
  // methods refuses the same way.
  const auto cross_method = borrowed_first.AddSession("PUT", "/c", SessionRecorder(nullptr, ""));
  ASSERT_FALSE(cross_method.ok());
  EXPECT_NE(cross_method.error().message().find("one seam"), std::string::npos);

  // The seam refusal outranks argument errors (the categorical mistake is
  // reported first), and a refusal leaves the router intact: existing
  // routes still gate, and same-seam adds still work.
  const auto bad_pattern_too =
      borrowed_first.AddSession("GET", "no-slash", SessionRecorder(nullptr, ""));
  ASSERT_FALSE(bad_pattern_too.ok());
  EXPECT_NE(bad_pattern_too.error().message().find("one seam"), std::string::npos);
  EXPECT_EQ(borrowed_first.Gate()(Request("GET", "/a")), std::nullopt);
  EXPECT_TRUE(borrowed_first.Add("PUT", "/c", Recorder(nullptr, "")).ok());
}

TEST(WebSocketRouterTest, AddSessionRejectsBadPatternsAndConflictsLikeAdd) {
  // AddSession shares Add's parse/conflict tail; this pins the shared
  // behavior from the session entrance, plus the failed-add-latches-
  // nothing rule the header states.
  WebSocketRouter router;
  EXPECT_FALSE(router.AddSession("GET", "no-slash", SessionRecorder(nullptr, "")).ok());
  EXPECT_FALSE(router.AddSession("GET", "/rooms/{}", SessionRecorder(nullptr, "")).ok());
  EXPECT_FALSE(router.AddSession("GET", "/files/{path+}/tail", SessionRecorder(nullptr, "")).ok());

  ASSERT_TRUE(router.AddSession("GET", "/rooms/{roomId}", SessionRecorder(nullptr, "")).ok());
  const auto conflict = router.AddSession("GET", "/rooms/{other}", SessionRecorder(nullptr, ""));
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().kind(), ErrorKind::kValidation);
  EXPECT_TRUE(router.AddSession("PUT", "/rooms/{roomId}", SessionRecorder(nullptr, "")).ok());

  // A failed borrowed Add latches no seam, so the shared seam stays open.
  WebSocketRouter unlatched;
  EXPECT_FALSE(unlatched.Add("GET", "no-slash", Recorder(nullptr, "")).ok());
  EXPECT_TRUE(unlatched.AddSession("GET", "/a", SessionRecorder(nullptr, "")).ok());
}

TEST(WebSocketRouterTest, AddRejectsBadPatternsAndConflictsLikeRouter) {
  WebSocketRouter router;
  EXPECT_FALSE(router.Add("GET", "no-slash", Recorder(nullptr, "")).ok());
  EXPECT_FALSE(router.Add("GET", "/rooms/{}", Recorder(nullptr, "")).ok());
  EXPECT_FALSE(router.Add("GET", "/files/{path+}/tail", Recorder(nullptr, "")).ok());

  ASSERT_TRUE(router.Add("GET", "/rooms/{roomId}", Recorder(nullptr, "")).ok());
  // Same shape (a label matches whatever the other label matches): conflict.
  const auto conflict = router.Add("GET", "/rooms/{other}", Recorder(nullptr, ""));
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().kind(), ErrorKind::kValidation);
  // A different method is a different route, like Router.
  EXPECT_TRUE(router.Add("PUT", "/rooms/{roomId}", Recorder(nullptr, "")).ok());
}

}  // namespace
}  // namespace opal::server
