// The streaming halves of the compile harness, behaviorally (ADR-0016). A
// separate target from gauntlet_compile_test because generated streaming
// clients fall back to opal::http::BeastWebSocketClient::Dialer(), so
// linking them pulls in Boost — this test joins the Beast targets behind the
// documented local exclusion list (docs/development.md); CI runs it. The
// injected in-memory dialer below is the ClientConfig::websocket_dialer seam
// working as designed: streams without a wire, exactly how consumer tests
// run them.
//
// Beyond compile coverage this drives the generated cbor streaming SERVER
// (never constructed anywhere else), the REST streaming route's
// validation-refusal arm (no other fixture has a constrained initial
// member), and the auth traits' upgrade-dial wiring. Link note: the client
// and server libraries of one service each carry an identical serde.cc; as
// static archives the linker resolves every serde symbol from the first and
// never pulls the second (the gauntlet_compile_test precedent) — compiling
// both serde TUs into one binary by hand would double-define them.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compile/streaming/cbor/client.h"
#include "compile/streaming/cbor/server.h"
#include "compile/streaming/jsonrpc/client.h"
#include "compile/streaming/jsonrpc/server.h"
#include "compile/streaming/rest/client.h"
#include "compile/streaming/rest/server.h"
#include "opal/cbor/cbor.h"
#include "opal/client/config.h"
#include "opal/core/document.h"
#include "opal/eventstream/envelope.h"
#include "opal/http/loopback.h"
#include "opal/http/message.h"
#include "opal/http/websocket.h"
#include "opal/http/websocket_pair.h"

namespace {

namespace relay = compile::streaming::rest;
namespace pipe = compile::streaming::cbor;
namespace wire = compile::streaming::jsonrpc;

// Scope-exit join for a blocking-seam serve thread, closing the client end
// first so the thread's blocked Receive drains: an ASSERT's early return
// must neither terminate on a joinable thread nor deadlock the join.
struct ServeJoin {
  std::shared_ptr<opal::http::WebSocket> peer;
  std::thread thread;
  ~ServeJoin() {
    peer->Close();
    if (thread.joinable()) thread.join();
  }
};

// A dialer handing out one end of an in-memory pair, keeping the other for
// the test to play the server and the full dial request for assertions.
opal::ClientConfig InMemoryConfig(std::shared_ptr<opal::http::WebSocket>* server_end,
                                  opal::http::WebSocketDialRequest* dialed) {
  opal::ClientConfig config;
  config.endpoint = "http://localhost:8080";
  config.websocket_dialer = [server_end, dialed](const opal::http::WebSocketDialRequest& request)
      -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
    *dialed = request;
    auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
    *server_end = std::move(far);
    return near;
  };
  return config;
}

TEST(StreamingCompileTest, RestClientOpensAServerPushStream) {
  std::shared_ptr<opal::http::WebSocket> server_end;
  opal::http::WebSocketDialRequest dialed;
  auto client = relay::RelayClient::Create(InMemoryConfig(&server_end, &dialed));
  ASSERT_TRUE(client.ok());

  auto stream = client->Watch();
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/watch");

  server_end->Close();
  auto received = stream->Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());  // the peer's clean close
}

TEST(StreamingCompileTest, TheUpgradeDialCarriesAuthQueryAndHeaderBindings) {
  // The service's auth traits ride the upgrade dial exactly like a unary
  // request: @httpBearerAuth attaches the configured token, and the modeled
  // query/header initial members resolve onto the dial request.
  std::shared_ptr<opal::http::WebSocket> server_end;
  opal::http::WebSocketDialRequest dialed;
  opal::ClientConfig config = InMemoryConfig(&server_end, &dialed);
  config.bearer_token = [] { return std::string("sesame"); };
  auto client = relay::RelayClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  relay::ConverseInput input;
  input.room = "lobby";
  input.since = 5;
  input.client = "probe";
  auto stream = client->Converse(input);
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/rooms/lobby/converse?since=5");
  EXPECT_EQ(dialed.headers.Get("authorization").value_or(""), "Bearer sesame");
  EXPECT_EQ(dialed.headers.Get("x-relay-client").value_or(""), "probe");
  stream->Close();
}

TEST(StreamingCompileTest, CborClientExchangesOnTheFixedUpgradeUri) {
  std::shared_ptr<opal::http::WebSocket> server_end;
  opal::http::WebSocketDialRequest dialed;
  opal::ClientConfig config = InMemoryConfig(&server_end, &dialed);
  // @httpApiKeyAuth(in: "header"): the key rides the upgrade request.
  config.api_key = [] { return std::string("key-123"); };
  auto client = pipe::PipeClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  auto stream = client->Exchange({});
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/service/Pipe/operation/Exchange");
  EXPECT_EQ(dialed.headers.Get("x-api-key").value_or(""), "key-123");

  pipe::ChatMessage message;
  message.text = "hello";
  message.sender = "ada";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());

  // The frame is the real envelope convention (ADR-0016): :event-type names
  // the engaged member, and the payload is the protocol's CBOR.
  auto frame = server_end->Receive();
  ASSERT_TRUE(frame.ok());
  ASSERT_TRUE(frame->has_value());
  auto envelope = opal::eventstream::ParseEnvelope(**frame);
  ASSERT_TRUE(envelope.ok());
  EXPECT_EQ(envelope->kind, opal::eventstream::EventEnvelope::Kind::kEvent);
  EXPECT_EQ(envelope->type, "message");
  EXPECT_EQ(envelope->content_type, "application/cbor");
  auto payload = opal::cbor::Decode(envelope->payload);
  ASSERT_TRUE(payload.ok());
  ASSERT_TRUE(payload->is_map());
  const opal::Document* text = payload->Find("text");
  ASSERT_NE(text, nullptr);
  ASSERT_TRUE(text->is_string());
  EXPECT_EQ(text->as_string(), "hello");
  const opal::Document* sender = payload->Find("sender");
  ASSERT_NE(sender, nullptr);
  ASSERT_TRUE(sender->is_string());
  EXPECT_EQ(sender->as_string(), "ada");

  // The other direction: the test plays the server, minting the same
  // envelope shape; the generated decoder yields the typed event.
  opal::DocumentMap joined;
  joined.emplace("member", opal::Document("ada"));
  ASSERT_TRUE(
      server_end
          ->Send(opal::eventstream::MakeEventMessage(
              "joined", "application/cbor", opal::cbor::Encode(opal::Document(std::move(joined)))))
          .ok());
  auto received = stream->Receive();
  ASSERT_TRUE(received.ok());
  ASSERT_TRUE(received->has_value());
  ASSERT_TRUE((**received).is_joined());
  EXPECT_EQ((**received).as_joined().member, "ada");

  stream->Close();
}

TEST(StreamingCompileTest, JsonRpcClientOpensOnTheSharedEndpoint) {
  // The ADR-0023 face: the dial targets the protocol's shared endpoint on
  // the raw-text wire, auth rides the upgrade like everywhere else, and
  // the opening request envelope carries the initial-request members.
  std::shared_ptr<opal::http::WebSocket> server_end;
  opal::http::WebSocketDialRequest dialed;
  opal::ClientConfig config = InMemoryConfig(&server_end, &dialed);
  config.bearer_token = [] { return std::string("sesame"); };
  auto client = wire::WireClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  wire::ConferInput input;
  input.room = "lobby";
  auto stream = client->Confer(input);
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/");
  EXPECT_TRUE(dialed.raw_text_frames);
  EXPECT_EQ(dialed.headers.Get("authorization").value_or(""), "Bearer sesame");

  auto opening = server_end->Receive();
  ASSERT_TRUE(opening.ok() && opening->has_value());
  EXPECT_TRUE((**opening).headers.empty());
  EXPECT_EQ((**opening).payload.ToString(),
            R"({"id":1,"jsonrpc":"2.0","method":"Confer","params":{"room":"lobby"}})");
  stream->Close();
}

TEST(StreamingCompileTest, JsonRpcWatchOpensWithEmptyParams) {
  // The Unit-input face: no params to serialize, still an (empty) params
  // member on the opening envelope; the transmit direction is NoEvents.
  std::shared_ptr<opal::http::WebSocket> server_end;
  opal::http::WebSocketDialRequest dialed;
  auto client = wire::WireClient::Create(InMemoryConfig(&server_end, &dialed));
  ASSERT_TRUE(client.ok());

  auto stream = client->Watch();
  ASSERT_TRUE(stream.ok());
  auto opening = server_end->Receive();
  ASSERT_TRUE(opening.ok() && opening->has_value());
  EXPECT_EQ((**opening).payload.ToString(),
            R"({"id":1,"jsonrpc":"2.0","method":"Watch","params":{}})");
  server_end->Close();
  auto received = stream->Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());  // the peer's clean close
}

// ---------------------------------------------------------------------------
// The streaming route's validation-refusal arm: no other fixture has a
// constrained initial member (streaming.smithy's @length on the room label
// exists for exactly this). Serves an over-long label through the generated
// StreamRouter and expects one SerializationException exception message,
// then the close — and a handler that never ran.
// ---------------------------------------------------------------------------

class NeverCalledHandler final : public relay::RelayHandler {
 public:
  opal::Outcome<opal::Unit> Converse(const relay::ConverseInput&, relay::ConverseServerStream&,
                                     const opal::server::RequestContext&) override {
    ADD_FAILURE() << "handler ran despite a validation failure";
    return opal::Unit{};
  }
  opal::Outcome<opal::Unit> Watch(const relay::WatchInput&, relay::WatchServerStream& stream,
                                  const opal::server::RequestContext&) override {
    stream.Close();
    return opal::Unit{};
  }
};

TEST(StreamValidationTest, AnInvalidLabelRefusesWithOneExceptionThenTheClose) {
  relay::RelayServer server(std::make_shared<NeverCalledHandler>());
  auto [client_end, server_end] = opal::http::InMemoryWebSocketPair::Create();
  opal::http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/rooms/waytoolongroom/converse";  // @length(max: 8) violated
  ServeJoin serve{client_end, std::thread([serve = server.StreamRouter()->Serve(), upgrade,
                                           session = server_end] { serve(upgrade, *session); })};

  auto refusal = client_end->Receive();
  ASSERT_TRUE(refusal.ok() && refusal->has_value());
  auto envelope = opal::eventstream::ParseEnvelope(**refusal);
  ASSERT_TRUE(envelope.ok()) << envelope.error().message();
  EXPECT_EQ(envelope->kind, opal::eventstream::EventEnvelope::Kind::kException);
  EXPECT_EQ(envelope->type, "SerializationException");

  auto closed = client_end->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
}

// The ADR-0021 twin: the never-launched async handler behind the session
// seam's identical refusal — before any coroutine exists.
class NeverCalledAsyncHandler final : public relay::RelayAsyncHandler {
 public:
  opal::eventstream::StreamTask Converse(relay::ConverseInput,
                                         relay::ConverseAsyncServerStream&) override {
    ADD_FAILURE() << "async handler launched despite a validation failure";
    co_return opal::Unit{};
  }
  opal::eventstream::StreamTask Watch(relay::WatchInput,
                                      relay::WatchAsyncServerStream& stream) override {
    (void)co_await stream.Receive();
    co_return opal::Unit{};
  }
};

TEST(StreamValidationTest, TheSessionSeamRefusesAnInvalidLabelIdentically) {
  relay::RelayServer server(std::make_shared<NeverCalledAsyncHandler>());
  auto [client_end, server_end] = opal::http::InMemoryWebSocketPair::Create();
  opal::http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/rooms/waytoolongroom/converse";  // @length(max: 8) violated
  // The launch point refuses inline — no serve thread, no coroutine.
  server.StreamRouter()->ServeSession()(upgrade, server_end);

  auto refusal = client_end->Receive();
  ASSERT_TRUE(refusal.ok() && refusal->has_value());
  auto envelope = opal::eventstream::ParseEnvelope(**refusal);
  ASSERT_TRUE(envelope.ok()) << envelope.error().message();
  EXPECT_EQ(envelope->kind, opal::eventstream::EventEnvelope::Kind::kException);
  EXPECT_EQ(envelope->type, "SerializationException");

  auto closed = client_end->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
}

// The jsonRpc2 twins: the same constrained member refused on the JSON-RPC
// wire (ADR-0023) — a ValidationException error envelope for the opening
// call's id, then the close. The opening params carry the member, so the
// refusal happens after the first message, not at the upgrade.
class NeverCalledWireHandler final : public wire::WireHandler {
 public:
  opal::Outcome<opal::Unit> Confer(const wire::ConferInput&, wire::ConferServerStream&,
                                   const opal::server::RequestContext&) override {
    ADD_FAILURE() << "handler ran despite a validation failure";
    return opal::Unit{};
  }
  opal::Outcome<opal::Unit> Watch(const wire::WatchInput&, wire::WatchServerStream& stream,
                                  const opal::server::RequestContext&) override {
    stream.Close();
    return opal::Unit{};
  }
};

class NeverCalledAsyncWireHandler final : public wire::WireAsyncHandler {
 public:
  opal::eventstream::StreamTask Confer(wire::ConferInput, wire::ConferAsyncServerStream&) override {
    ADD_FAILURE() << "async handler launched despite a validation failure";
    co_return opal::Unit{};
  }
  opal::eventstream::StreamTask Watch(wire::WatchInput,
                                      wire::WatchAsyncServerStream& stream) override {
    (void)co_await stream.Receive();
    co_return opal::Unit{};
  }
};

TEST(StreamValidationTest, TheJsonRpcWireRefusesTheSameConstraintOnBothSeams) {
  const std::string opening =
      R"({"jsonrpc":"2.0","method":"Confer","params":{"room":"waytoolongroom"},"id":5})";
  opal::http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/";

  // Session seam: the driver reads the opening inside its Detached frame,
  // refuses, and closes — inline over the pair.
  wire::WireServer async_server(std::make_shared<NeverCalledAsyncWireHandler>());
  auto [async_client_end, async_server_end] = opal::http::InMemoryWebSocketPair::Create();
  async_server.StreamRouter()->ServeSession()(upgrade, async_server_end);
  opal::eventstream::Message open_message;
  open_message.payload = opal::Blob::FromString(opening);
  ASSERT_TRUE(async_client_end->Send(open_message).ok());
  auto refusal = async_client_end->Receive();
  ASSERT_TRUE(refusal.ok() && refusal->has_value());
  const std::string text = (**refusal).payload.ToString();
  EXPECT_NE(text.find("\"code\":400"), std::string::npos) << text;
  EXPECT_NE(text.find("ValidationException"), std::string::npos) << text;
  EXPECT_NE(text.find("\"id\":5"), std::string::npos) << text;
  auto closed = async_client_end->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());

  // Blocking seam: identical bytes through the served thread.
  wire::WireServer blocking_server(std::make_shared<NeverCalledWireHandler>());
  auto [client_end, server_end] = opal::http::InMemoryWebSocketPair::Create();
  ServeJoin serve{client_end, std::thread([serve = blocking_server.StreamRouter()->Serve(), upgrade,
                                           session = server_end] { serve(upgrade, *session); })};
  ASSERT_TRUE(client_end->Send(open_message).ok());
  auto blocking_refusal = client_end->Receive();
  ASSERT_TRUE(blocking_refusal.ok() && blocking_refusal->has_value());
  EXPECT_EQ((**blocking_refusal).payload.ToString(), text);  // seam parity, byte for byte
  auto blocking_closed = client_end->Receive();
  ASSERT_TRUE(blocking_closed.ok());
  EXPECT_FALSE(blocking_closed->has_value());
}

// ---------------------------------------------------------------------------
// The generated rpcv2Cbor streaming SERVER, behaviorally: nothing else ever
// constructs a PipeServer or serves its StreamRouter (gauntlet_compile_test
// only compiles it). The generated cbor client drives it through the
// in-memory pair: the fixed upgrade URI route, both codec directions over
// CBOR payloads, and the mid-stream RoomGone exception round trip.
// ---------------------------------------------------------------------------

// Echoes messages back prefixed until one says "gone", then fails modeled.
class EchoHandler final : public pipe::PipeHandler {
 public:
  opal::Outcome<opal::Unit> Exchange(const pipe::ExchangeInput&, pipe::ExchangeServerStream& stream,
                                     const opal::server::RequestContext&) override {
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) return opal::Unit{};
      if (!(**event).is_message()) continue;
      const std::string& text = (**event).as_message().text;
      if (text == "gone") {
        opal::Error gone = opal::Error::Modeled("RoomGone", "the room left");
        gone.set_detail(pipe::RoomGone{.message = "the room left"});
        return gone;
      }
      pipe::ChatMessage echo;
      echo.text = "echo:" + text;
      if (!stream.Send(pipe::ServerEvents::FromMessage(echo)).ok()) return opal::Unit{};
    }
  }

  opal::Outcome<opal::Unit> Watch(const pipe::WatchInput&, pipe::WatchServerStream& stream,
                                  const opal::server::RequestContext&) override {
    pipe::ChatMessage message;
    message.text = "pushed";
    (void)stream.Send(pipe::ServerEvents::FromMessage(message));
    return opal::Unit{};
  }
};

// The async EchoHandler (ADR-0021): the same wire behavior, co_awaited.
class EchoAsyncHandler final : public pipe::PipeAsyncHandler {
 public:
  opal::eventstream::StreamTask Exchange(pipe::ExchangeInput,
                                         pipe::ExchangeAsyncServerStream& stream) override {
    while (true) {
      auto event = co_await stream.Receive();
      if (!event.ok() || !event->has_value()) co_return opal::Unit{};
      if (!(**event).is_message()) continue;
      const std::string& text = (**event).as_message().text;
      if (text == "gone") {
        opal::Error gone = opal::Error::Modeled("RoomGone", "the room left");
        gone.set_detail(pipe::RoomGone{.message = "the room left"});
        co_return gone;
      }
      pipe::ChatMessage echo;
      echo.text = "echo:" + text;
      if (!(co_await stream.Send(pipe::ServerEvents::FromMessage(echo))).ok()) {
        co_return opal::Unit{};
      }
    }
  }

  opal::eventstream::StreamTask Watch(pipe::WatchInput,
                                      pipe::WatchAsyncServerStream& stream) override {
    pipe::ChatMessage message;
    message.text = "pushed";
    (void)co_await stream.Send(pipe::ServerEvents::FromMessage(message));
    co_return opal::Unit{};
  }
};

class CborStreamEndToEndTest : public testing::Test {
 protected:
  void SetUp() override {
    Start(std::make_unique<pipe::PipeServer>(std::make_shared<EchoHandler>()),
          /*session_seam=*/false);
  }

  // Rewires around a different server mid-test (the chat fixture's shape);
  // earlier sessions stay owned until TearDown.
  void Start(std::unique_ptr<pipe::PipeServer> server, bool session_seam) {
    server_ = std::move(server);
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = std::make_shared<opal::http::Loopback>();
    config.websocket_dialer = [this, session_seam](const opal::http::WebSocketDialRequest& request)
        -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
      dialed_target_ = request.target;
      auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
      opal::http::HttpRequest upgrade;
      upgrade.method = "GET";
      upgrade.target = request.target;
      upgrade.headers = request.headers;
      sessions_.push_back(far);
      if (session_seam) {
        server_->StreamRouter()->ServeSession()(upgrade, far);  // a launch point, not a loop
      } else {
        threads_.emplace_back([serve = server_->StreamRouter()->Serve(), upgrade, session = far] {
          serve(upgrade, *session);
        });
      }
      return near;
    };
    auto client = pipe::PipeClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<pipe::PipeClient>(std::move(*client));
  }

  void TearDown() override {
    for (auto& session : sessions_) session->Close();
    for (std::thread& thread : threads_) thread.join();
  }

  std::unique_ptr<pipe::PipeServer> server_;
  std::unique_ptr<pipe::PipeClient> client_;
  std::vector<std::shared_ptr<opal::http::WebSocket>> sessions_;
  std::vector<std::thread> threads_;
  std::string dialed_target_;
};

TEST_F(CborStreamEndToEndTest, BidiCborEventsRoundTripThroughTheGeneratedServer) {
  auto stream = client_->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(dialed_target_, "/service/Pipe/operation/Exchange");

  pipe::ChatMessage message;
  message.text = "hello";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());
  auto echo = stream->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value()) << (echo.ok() ? "closed" : echo.error().message());
  ASSERT_TRUE((**echo).is_message());
  EXPECT_EQ((**echo).as_message().text, "echo:hello");

  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(CborStreamEndToEndTest, RoomGoneRoundTripsAsATypedMidStreamError) {
  auto stream = client_->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  pipe::ChatMessage message;
  message.text = "gone";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "RoomGone");
  EXPECT_EQ(outcome.error().message(), "the room left");
  const pipe::RoomGone* detail = outcome.error().detail<pipe::RoomGone>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->message.value_or(""), "the room left");
}

TEST_F(CborStreamEndToEndTest, TheAsyncConstructorServesTheSameCborWire) {
  // The ADR-0021 seam on the cbor protocol: the same fixed-URI route,
  // codecs, echo, and typed mid-stream error — zero serve threads.
  const std::size_t threads_before = threads_.size();
  Start(std::make_unique<pipe::PipeServer>(std::make_shared<EchoAsyncHandler>()),
        /*session_seam=*/true);

  auto stream = client_->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(dialed_target_, "/service/Pipe/operation/Exchange");
  EXPECT_EQ(threads_.size(), threads_before);  // the launch point spawned none

  pipe::ChatMessage message;
  message.text = "hello";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());
  auto echo = stream->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value()) << (echo.ok() ? "closed" : echo.error().message());
  ASSERT_TRUE((**echo).is_message());
  EXPECT_EQ((**echo).as_message().text, "echo:hello");

  message.text = "gone";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().code(), "RoomGone");
  ASSERT_NE(outcome.error().detail<pipe::RoomGone>(), nullptr);
}

TEST_F(CborStreamEndToEndTest, WatchPushesOverTheFixedUri) {
  auto stream = client_->Watch();
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(dialed_target_, "/service/Pipe/operation/Watch");
  auto update = stream->Receive();
  ASSERT_TRUE(update.ok() && update->has_value());
  ASSERT_TRUE((**update).is_message());
  EXPECT_EQ((**update).as_message().text, "pushed");
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

}  // namespace
