// Out-of-tree acceptance for the WebSocket transports (ADR-0015, the
// definition-of-done e2e ADR-0014 pinned): a consumer wires a streaming
// server — gate and serve callback — through the module boundary, dials it
// with the runtime's own client, and drains real event-stream frames both
// ways — bounded receives included, the deadline a consumer's suite leans
// on so a missing event fails instead of hanging. This is the wiring
// applications use ahead of slice 3's generated EventStream API.

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

#include "opal/eventstream/frame.h"
#include "opal/http/beast_transport.h"
#include "opal/http/websocket.h"

namespace {

using opal::eventstream::Message;
using opal::http::BeastServerTransport;
using opal::http::BeastWebSocketClient;
using opal::http::HttpRequest;
using opal::http::HttpResponse;
using opal::http::WebSocket;

Message Event(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = opal::Blob::FromString(body)};
}

TEST(WebSocketAcceptanceTest, AConsumerServesAndDrainsAStreamThroughTheModuleBoundary) {
  BeastServerTransport::Options options;
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.headers.Get("authorization").value_or("") != "Bearer consumer-token") {
      HttpResponse refusal;
      refusal.status = 401;
      return refusal;
    }
    return std::nullopt;
  };
  options.on_websocket = [](const HttpRequest& request, WebSocket& socket) {
    // Greet, then echo until the client closes — dispatching on the
    // FindString lookup the codec ships.
    (void)socket.Send(Event("greeting", "hello " + request.target));
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        return;
      }
      const std::string* kind = (*message)->FindString(":event-type");
      Message reply =
          Event(kind != nullptr ? *kind : "unknown", "echo:" + (*message)->payload.ToString());
      if (!socket.Send(reply).ok()) {
        return;
      }
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 404;
                    return response;
                  })
                  .ok());

  // An unauthenticated dial is refused before any upgrade exists.
  EXPECT_FALSE(BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()}).ok());

  opal::http::Headers credentials;
  credentials.Add("authorization", "Bearer consumer-token");
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .target = "/events", .headers = credentials});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const auto& socket = *dialed;

  auto greeting = socket->Receive();
  ASSERT_TRUE(greeting.ok() && greeting->has_value());
  EXPECT_EQ((**greeting).payload.ToString(), "hello /events");

  for (int i = 0; i < 5; ++i) {
    const std::string body = "consumer-" + std::to_string(i);
    ASSERT_TRUE(socket->Send(Event("chat", body)).ok());
    auto echo = socket->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_NE((**echo).FindString(":event-type"), nullptr);
    EXPECT_EQ(*(**echo).FindString(":event-type"), "chat");
    EXPECT_EQ((**echo).payload.ToString(), "echo:" + body);
  }

  socket->Close();
  auto end = socket->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
  server.Stop();
}

TEST(WebSocketAcceptanceTest, AConsumerBoundsAReceiveAndKeepsTheSession) {
  // The bounded read a consumer's test suite wants: a stream where the
  // expected event never arrives fails with a verdict instead of hanging
  // the job, and the session is still there to assert against afterwards.
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    // Deliberately quiet until spoken to — the peer a consumer's wrong
    // expectation waits on forever without a deadline.
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) return;
      if (!socket.Send(Event("pong", "echo:" + (*message)->payload.ToString())).ok()) return;
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 404;
                    return response;
                  })
                  .ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const auto& socket = *dialed;

  auto nothing = socket->Receive(std::chrono::milliseconds(100));
  ASSERT_FALSE(nothing.ok());
  // "TimeoutError" is what tells a consumer "nothing arrived yet" apart
  // from a clean close (nullopt) and a dead wire (TransportError).
  EXPECT_EQ(nothing.error().code(), "TimeoutError");

  // Carrying on is the whole point: Close() would have ended the stream.
  ASSERT_TRUE(socket->Send(Event("ping", "still here")).ok());
  auto echo = socket->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:still here");

  socket->Close();
  server.Stop();
}

}  // namespace
