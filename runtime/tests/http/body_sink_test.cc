// The BodySink contract (issue #213) at the level every transport shares:
// HttpClient::SendStreaming's default implementation, which buffers and then
// hands the body over in one piece. It is the fallback a transport that
// cannot stream inherits, and the point of the tests here is that a caller
// cannot tell the difference from the outside — same delivery, same empty
// HttpResponse::body, same failure on an aborted sink. What only a streaming
// transport gives is memory: the pieces, and not holding the body at all.
// That part is BeastHttpClient's, and beast_client_test.cc pins it.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "opal/http/loopback.h"
#include "opal/http/socket_transport.h"
#include "opal/http/transport.h"

namespace opal::http {
namespace {

// Records what a sink was handed, and answers accept/write however a test
// asks it to.
struct RecordingSink {
  std::vector<std::string> pieces;
  int accept_calls = 0;
  bool take = true;  // what accept() answers
  bool keep = true;  // what write() answers
  int seen_status = 0;

  BodySink Sink() {
    return BodySink{
        .accept =
            [this](int status, const Headers&) {
              ++accept_calls;
              seen_status = status;
              return take;
            },
        .write =
            [this](std::string_view piece) {
              pieces.emplace_back(piece);
              return keep;
            },
    };
  }

  std::string Joined() const {
    std::string all;
    for (const auto& piece : pieces) all += piece;
    return all;
  }
};

Loopback Echoing(std::string body, int status = 200) {
  Loopback loopback;
  (void)loopback.Start([body = std::move(body), status](const HttpRequest&) {
    HttpResponse response;
    response.status = status;
    response.headers.Set("content-type", "text/plain");
    response.body = body;
    return response;
  });
  return loopback;
}

TEST(BodySinkTest, AnAcceptedBodyReachesTheSinkAndLeavesTheResponseEmpty) {
  Loopback transport = Echoing("the whole body");
  RecordingSink sink;

  const auto response = transport.SendStreaming(HttpRequest{}, sink.Sink());
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  // The headers survive — a streamed call is still a response, and the
  // caller needs them to decide what the bytes are.
  EXPECT_EQ(response->headers.Get("content-type").value_or(""), "text/plain");
  EXPECT_EQ(sink.Joined(), "the whole body");
  EXPECT_TRUE(response->body.empty()) << "the body was delivered twice";
  EXPECT_EQ(sink.accept_calls, 1);
  EXPECT_EQ(sink.seen_status, 200);
}

TEST(BodySinkTest, ADeclinedBodyStaysInTheResponse) {
  // accept() is per response so a caller can take the payload and leave the
  // error document where the rest of the client already knows to look.
  Loopback transport = Echoing("{\"message\":\"no such thing\"}", 404);
  RecordingSink sink;
  sink.take = false;

  const auto response = transport.SendStreaming(HttpRequest{}, sink.Sink());
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 404);
  EXPECT_EQ(response->body, "{\"message\":\"no such thing\"}");
  EXPECT_EQ(sink.accept_calls, 1);
  EXPECT_TRUE(sink.pieces.empty());
}

TEST(BodySinkTest, ASinkThatAbortsFailsTheCallAndIsNotRetryable) {
  // The caller hit its own limit — a disk quota, a length it refuses to
  // exceed. Retrying would only deliver the same bytes again.
  Loopback transport = Echoing("payload");
  RecordingSink sink;
  sink.keep = false;

  const auto response = transport.SendStreaming(HttpRequest{}, sink.Sink());
  ASSERT_FALSE(response.ok());
  EXPECT_FALSE(response.error().retryable());
  EXPECT_NE(response.error().message().find("sink aborted"), std::string::npos)
      << response.error().message();
}

TEST(BodySinkTest, AnEmptyBodyNeverCallsWrite) {
  Loopback transport = Echoing("", 204);
  RecordingSink sink;

  const auto response = transport.SendStreaming(HttpRequest{}, sink.Sink());
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 204);
  EXPECT_EQ(sink.accept_calls, 1) << "accept is asked about every response, body or not";
  EXPECT_TRUE(sink.pieces.empty());
}

TEST(BodySinkTest, AnIncompleteSinkIsNoSinkAtAll) {
  // Half a sink is a programming error that would otherwise lose the body
  // silently; the response is buffered exactly as Send() buffers it.
  Loopback transport = Echoing("still here");

  RecordingSink sink;
  BodySink no_write = sink.Sink();
  no_write.write = nullptr;
  auto response = transport.SendStreaming(HttpRequest{}, no_write);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "still here");
  EXPECT_EQ(sink.accept_calls, 0) << "a sink that cannot be written to is never consulted";

  BodySink no_accept = sink.Sink();
  no_accept.accept = nullptr;
  response = transport.SendStreaming(HttpRequest{}, no_accept);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "still here");
  EXPECT_TRUE(sink.pieces.empty());
}

TEST(BodySinkTest, ATransportFailureNeverReachesTheSink) {
  Loopback transport;  // no handler installed
  RecordingSink sink;

  const auto response = transport.SendStreaming(HttpRequest{}, sink.Sink());
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(sink.accept_calls, 0);
  EXPECT_TRUE(sink.pieces.empty());
}

TEST(BodySinkTest, TheSocketTransportHonorsTheSameContract) {
  // The built-in socket client inherits the default implementation, so it is
  // bounded by max_response_bytes rather than by the sink — but a caller
  // writing against the sink API gets the same delivery from it.
  SocketHttpServer server;
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());

  HttpRequest request;
  request.method = "POST";
  request.target = "/echo";
  request.body = std::string(64 * 1024, 'x');

  RecordingSink sink;
  const auto response = client.SendStreaming(request, sink.Sink());
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(sink.Joined().size(), request.body.size());
  EXPECT_EQ(sink.Joined(), request.body);
  EXPECT_TRUE(response->body.empty());
  server.Stop();
}

}  // namespace
}  // namespace opal::http
