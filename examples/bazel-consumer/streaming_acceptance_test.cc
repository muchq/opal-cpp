// Out-of-tree acceptance for the generated streaming API (ADR-0016, slice
// 3's consumer e2e): a consumer's generated client runs a bidirectional
// round trip against its generated server's StreamRouter through the module
// boundary, the streams riding an injected InMemoryWebSocketPair dialer —
// the documented Boost-free way to test streams (no wire, no sockets; the
// serve callback is invoked directly with the synthesized upgrade request a
// real transport would deliver), with the typed bounded receive alongside
// it. websocket_acceptance_test covers the real-wire transport underneath.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "acme/chat/client.h"
#include "acme/chat/server.h"
#include "opal/client/config.h"
#include "opal/http/message.h"
#include "opal/http/websocket.h"
#include "opal/http/websocket_pair.h"

namespace {

// Echoes every note back with an "echo:" prefix until the client closes.
class EchoHandler final : public acme::chat::ChatHandler {
 public:
  opal::Outcome<opal::Unit> Exchange(
      const acme::chat::ExchangeInput&,
      opal::eventstream::EventStream<acme::chat::Notes, acme::chat::Notes>& stream,
      const opal::server::RequestContext&) override {
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) return opal::Unit{};
      acme::chat::Note reply;
      reply.text = "echo:" + (**event).as_note().text;
      if (!stream.Send(acme::chat::Notes::FromNote(reply)).ok()) return opal::Unit{};
    }
  }
};

class StreamingAcceptanceTest : public testing::Test {
 protected:
  void TearDown() override {
    // Close is idempotent; this unblocks the serve loop if a failed test
    // body left it mid-Receive, so the join cannot hang.
    if (server_session_ != nullptr) server_session_->Close();
    if (serve_thread_.joinable()) serve_thread_.join();
  }

  acme::chat::ChatServer server_{std::make_shared<EchoHandler>()};
  std::shared_ptr<opal::http::WebSocket> server_session_;
  std::thread serve_thread_;
};

TEST_F(StreamingAcceptanceTest, ABidiRoundTripCrossesTheModuleBoundary) {
  opal::ClientConfig config;
  config.endpoint = "http://localhost:8080";
  config.websocket_dialer = [this](const opal::http::WebSocketDialRequest& request)
      -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
    auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
    opal::http::HttpRequest upgrade;
    upgrade.method = "GET";
    upgrade.target = request.target;
    upgrade.headers = request.headers;
    server_session_ = far;
    serve_thread_ = std::thread([serve = server_.StreamRouter()->Serve(), upgrade, session = far] {
      serve(upgrade, *session);
    });
    return near;
  };
  auto client = acme::chat::ChatClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  auto stream = client->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  for (int i = 0; i < 3; ++i) {
    acme::chat::Note note;
    note.text = "note-" + std::to_string(i);
    ASSERT_TRUE(stream->Send(acme::chat::Notes::FromNote(note)).ok());
    auto echo = stream->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_TRUE((**echo).is_note());
    EXPECT_EQ((**echo).as_note().text, "echo:note-" + std::to_string(i));
  }

  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());  // the server's acknowledging clean close
}

TEST_F(StreamingAcceptanceTest, ABoundedReceiveFailsInsteadOfHangingOnAMissingEvent) {
  // The consumer-test shape the deadline is for: this handler only ever
  // answers what it is sent, so a suite expecting an unprompted event would
  // park in Receive() until the job's timeout killed it — no assertion, no
  // clue which expectation was wrong. Bounded, the same wait is a red test.
  opal::ClientConfig config;
  config.endpoint = "http://localhost:8080";
  config.websocket_dialer = [this](const opal::http::WebSocketDialRequest& request)
      -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
    auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
    opal::http::HttpRequest upgrade;
    upgrade.method = "GET";
    upgrade.target = request.target;
    upgrade.headers = request.headers;
    server_session_ = far;
    serve_thread_ = std::thread([serve = server_.StreamRouter()->Serve(), upgrade, session = far] {
      serve(upgrade, *session);
    });
    return near;
  };
  auto client = acme::chat::ChatClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  auto stream = client->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  auto nothing = stream->Receive(std::chrono::milliseconds(100));
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError")
      << "expected a bounded wait, got: " << nothing.error().message();

  // The stream survived its deadline, so the suite can go on to assert what
  // the service DOES do — the move Close() would have made impossible.
  acme::chat::Note note;
  note.text = "after the deadline";
  ASSERT_TRUE(stream->Send(acme::chat::Notes::FromNote(note)).ok());
  auto echo = stream->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).as_note().text, "echo:after the deadline");

  stream->Close();
}

}  // namespace
