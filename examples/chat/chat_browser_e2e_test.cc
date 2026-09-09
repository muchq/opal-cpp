// The issue-#113 story end to end (ADR-0018): a browser-fidelity peer —
// subprotocol offer, Origin header, JSON.stringify out, JSON.parse in,
// nothing a page's `new WebSocket(url, protocol)` cannot do — joins the
// GENERATED chat service over real loopback WebSockets. The server wiring
// is the production recipe: RequireOrigin chained ahead of the generated
// StreamRouter's gate, websocket_accept_json_frames on, and the untouched
// RoomHandler serving typed events with no idea the wire is text. A native
// generated client converses beside the browser on the same port, still
// binary.
//
// CI-only under Bazel behind a download-blocking proxy, like the other
// Beast targets (docs/development.md, "Machine-specific Bazel flags"); the
// same file compiles and runs against distro Boost + gtest with the g++
// recipe documented there.

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "example/chat/client.h"
#include "example/chat/server.h"
#include "opal/client/config.h"
#include "opal/eventstream/json_frame.h"
#include "opal/http/beast_transport.h"
#include "opal/server/origin_gate.h"
#include "room_handler.h"

namespace example::chat {
namespace {

const std::string kJsonToken(opal::eventstream::kJsonFramesSubprotocol);
constexpr char kAllowedOrigin[] = "https://muchq.com";

// What a page on kAllowedOrigin does, and nothing more: offer the
// subprotocol, let the browser stamp Origin, then JSON text both ways.
class BrowserPeer {
 public:
  BrowserPeer(int port, const std::string& target, const std::string& origin) : ws_(io_) {
    boost::asio::ip::tcp::resolver resolver(io_);
    boost::asio::connect(ws_.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
    ws_.set_option(boost::beast::websocket::stream_base::decorator(
        [origin](boost::beast::websocket::request_type& req) {
          req.set(boost::beast::http::field::sec_websocket_protocol, kJsonToken);
          req.set(boost::beast::http::field::origin, origin);
        }));
    ws_.handshake(response_, "127.0.0.1", target, ec_);
  }

  // The dial outcome a page observes: onopen (no error, token echoed) or
  // onerror (here with the refusal status a browser never shows).
  const boost::beast::error_code& ec() const { return ec_; }
  unsigned status() const { return response_.result_int(); }
  std::string selected_subprotocol() const {
    return std::string(response_[boost::beast::http::field::sec_websocket_protocol]);
  }

  void SendText(const std::string& text) {
    ws_.text(true);
    ws_.write(boost::asio::buffer(text));
  }

  // One received message — asserted text, the JSON wire's only legal kind.
  std::string ReadText() {
    boost::beast::flat_buffer buffer;
    ws_.read(buffer);
    EXPECT_FALSE(ws_.got_binary()) << "the JSON wire carries text frames";
    return boost::beast::buffers_to_string(buffer.data());
  }

  // Reads until the server's close frame and reports its code — exactly
  // what a page's onclose event carries (1000 normal, 1002 protocol
  // error); 0 when the stream broke without any close handshake.
  unsigned DrainToCloseCode() {
    boost::beast::flat_buffer buffer;
    boost::beast::error_code ec;
    while (!ec) {
      ws_.read(buffer, ec);
      buffer.consume(buffer.size());
    }
    return ec == boost::beast::websocket::error::closed ? ws_.reason().code : 0u;
  }

 private:
  boost::asio::io_context io_;
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
  boost::beast::websocket::response_type response_;
  boost::beast::error_code ec_;
};

class ChatBrowserEndToEndTest : public testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<ChatServer>(std::make_shared<RoomHandler>());
    opal::http::BeastServerTransport::Options options;
    // The production browser wiring (ADR-0018): origin allowlist first,
    // then the generated router's refusals; JSON-text negotiation on.
    options.websocket_gate =
        [origin = opal::server::RequireOrigin({kAllowedOrigin}),
         router = server_->StreamRouter()->Gate()](
            const opal::http::HttpRequest& request) -> std::optional<opal::http::HttpResponse> {
      if (auto refusal = origin(request)) return refusal;
      return router(request);
    };
    options.on_websocket = server_->StreamRouter()->Serve();
    options.websocket_accept_json_frames = true;
    transport_ = std::make_unique<opal::http::BeastServerTransport>(options);
    ASSERT_TRUE(transport_->Start(server_->Handler()).ok());
  }

  void TearDown() override {
    if (transport_ != nullptr) transport_->Stop();
  }

  std::unique_ptr<ChatServer> server_;
  std::unique_ptr<opal::http::BeastServerTransport> transport_;
};

TEST_F(ChatBrowserEndToEndTest, ABrowserConversesInJsonTextFrames) {
  BrowserPeer page(transport_->port(), "/rooms/lobby/converse", kAllowedOrigin);
  ASSERT_FALSE(page.ec()) << page.ec().message();
  ASSERT_EQ(page.selected_subprotocol(), kJsonToken);

  // The greeting, byte-pinned: this is exactly what onmessage receives.
  // No nickname — a browser cannot set the x-chat-nickname upgrade header,
  // and the modeled member is optional, so the handler's default applies.
  EXPECT_EQ(page.ReadText(), R"({"event":"joined","payload":{"member":"anonymous"}})");

  // Hand-typed JSON — member order and whitespace a JS object literal
  // produces, not the runtime's canonical dialect.
  page.SendText(R"({ "payload": {"text": "hello from a browser"}, "event": "message" })");
  EXPECT_EQ(
      page.ReadText(),
      R"({"event":"message","payload":{"sender":"anonymous","text":"hello from a browser"}})");

  page.SendText(R"({"event":"leave","payload":{}})");
  EXPECT_EQ(page.ReadText(), R"({"event":"left","payload":{"member":"anonymous"}})");
  EXPECT_EQ(page.DrainToCloseCode(), 1000u);  // onclose: wasClean, code 1000
}

TEST_F(ChatBrowserEndToEndTest, TheModeledErrorReachesThePageAsAnExceptionEnvelope) {
  BrowserPeer page(transport_->port(), "/rooms/lobby/converse", kAllowedOrigin);
  ASSERT_FALSE(page.ec()) << page.ec().message();
  ASSERT_EQ(page.ReadText(), R"({"event":"joined","payload":{"member":"anonymous"}})");

  page.SendText(R"({"event":"message","payload":{"text":"kick me"}})");
  // ADR-0016's terminal exception, transposed: the typed error arm arrives
  // as the "exception" envelope, then the close.
  EXPECT_EQ(page.ReadText(),
            R"({"exception":"Kicked","payload":{"by":"moderator","message":"kicked from lobby"}})");
  EXPECT_EQ(page.DrainToCloseCode(), 1000u);  // the exception is the message; the close is clean
}

TEST_F(ChatBrowserEndToEndTest, ServerPushStreamsAsJsonText) {
  BrowserPeer page(transport_->port(), "/rooms/lobby/watch", kAllowedOrigin);
  ASSERT_FALSE(page.ec()) << page.ec().message();
  ASSERT_EQ(page.selected_subprotocol(), kJsonToken);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(page.ReadText(),
              R"({"event":"message","payload":{"sender":"room","text":"lobby-update-)" +
                  std::to_string(i) + R"("}})");
  }
  EXPECT_EQ(page.DrainToCloseCode(), 1000u);
}

TEST_F(ChatBrowserEndToEndTest, AForeignPageIsRefusedBeforeAnyUpgradeExists) {
  BrowserPeer page(transport_->port(), "/rooms/lobby/converse", "https://evil.example");
  ASSERT_TRUE(page.ec()) << "the origin gate must refuse the upgrade";
  EXPECT_EQ(page.status(), 403u);
}

TEST_F(ChatBrowserEndToEndTest, AMalformedEnvelopeEndsTheSessionFailClosed) {
  BrowserPeer page(transport_->port(), "/rooms/lobby/converse", kAllowedOrigin);
  ASSERT_FALSE(page.ec()) << page.ec().message();
  ASSERT_EQ(page.ReadText(), R"({"event":"joined","payload":{"member":"anonymous"}})");
  // A hand-rolled "type"-switch dialect, not the envelope: the session
  // fails closed instead of guessing.
  page.SendText(R"({"type":"message","text":"hi"})");
  EXPECT_EQ(page.DrainToCloseCode(), 1002u);  // onclose: code 1002, protocol error
}

TEST_F(ChatBrowserEndToEndTest, ANativeClientStaysBinaryBesideTheBrowser) {
  // The generated client never offers the subprotocol: same port, same
  // gate chain (no Origin header — the allowlist only judges browsers),
  // binary wire as always.
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.endpoint = "http://127.0.0.1:" + std::to_string(transport_->port());
  auto client = ChatClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  auto joined = stream->Receive();
  ASSERT_TRUE(joined.ok() && joined->has_value());
  ASSERT_TRUE((**joined).is_joined());
  EXPECT_EQ((**joined).as_joined().member, "ada");
  stream->Close();
}

}  // namespace
}  // namespace example::chat
