// Phase 8 slice 3 e2e, in-memory half (ADR-0016): the generated ChatClient
// drives the generated ChatServer's StreamRouter through an injected
// InMemoryWebSocketPair dialer — every streaming flow (bidi round trips,
// server push, clean closes both directions, the typed mid-stream error)
// plus the unary neighbor and the bounded receive, with no wire underneath.
// The dialer hands the client one end of the pair and serves the other end
// by invoking the router's Serve() callback directly with the synthesized
// upgrade request, exactly how the serve callback runs behind a real
// transport.
//
// chat_e2e_beast_test.cc is the real-WebSocket half (the PLAN §Phase 8 exit
// criterion). Note the link reality: a generated streaming client carries
// the default Beast dialer's definition (//runtime:http_beast), so even this
// wire-free binary joins the Beast targets behind the documented local
// exclusion list (docs/development.md); CI runs it everywhere.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example/chat/client.h"
#include "example/chat/server.h"
#include "opal/client/config.h"
#include "opal/core/blob.h"
#include "opal/eventstream/envelope.h"
#include "opal/http/loopback.h"
#include "opal/http/message.h"
#include "opal/http/websocket.h"
#include "opal/http/websocket_pair.h"
#include "room_handler.h"
#include "stream_test_fixture.h"

namespace example::chat {
namespace {

// The shared in-memory fixture (stream_test_fixture.h), seeded with the
// reference RoomHandler.
class ChatEndToEndTest : public StreamTestFixture {
 protected:
  void SetUp() override { StartWith(std::make_shared<RoomHandler>()); }
};

TEST_F(ChatEndToEndTest, BidiEventsRoundTripThroughTheGeneratedPair) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  // The upgrade target resolved the @http bindings exactly like a unary call.
  EXPECT_EQ(last_dialed_target_, "/rooms/lobby/converse");

  auto joined = stream->Receive();
  ASSERT_TRUE(joined.ok() && joined->has_value());
  ASSERT_TRUE((**joined).is_joined());
  EXPECT_EQ((**joined).as_joined().member, "ada");

  for (int i = 0; i < 5; ++i) {
    ChatMessage message;
    message.text = "hello-" + std::to_string(i);
    message.sender = "ada";
    ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
    auto echo = stream->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_TRUE((**echo).is_message());
    EXPECT_EQ((**echo).as_message().text, "hello-" + std::to_string(i));
    EXPECT_EQ((**echo).as_message().sender, "ada");
  }

  // Client-initiated close: the handler sees the clean end and returns; the
  // acknowledging close surfaces here as Receive's nullopt.
  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatEndToEndTest, ABoundedReceiveNamesTheMissingEventInsteadOfHanging) {
  // The failure this overload exists for, in the shape a suite meets it:
  // the room only answers what it is told, so an expectation of an
  // unprompted event is wrong — and without a deadline, "wrong" is a hung
  // job instead of a red test.
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  auto joined = stream->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(joined.ok()) << joined.error().message();
  ASSERT_TRUE(joined->has_value());
  ASSERT_TRUE((**joined).is_joined());

  auto nothing = stream->Receive(std::chrono::milliseconds(100));
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError")
      << "no second greeting is coming; the wait must end: " << nothing.error().message();

  // The session outlived the deadline, so the test carries on to what the
  // room really does — the assertion Close() would have made impossible.
  ChatMessage message;
  message.text = "still connected";
  message.sender = "ada";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto echo = stream->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  ASSERT_TRUE((**echo).is_message());
  EXPECT_EQ((**echo).as_message().text, "still connected");

  stream->Close();
  auto end = stream->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());  // the close, still not a deadline
}

TEST_F(ChatEndToEndTest, ServerEndsTheStreamAfterALeaveEvent) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "grace";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  LeaveNotice leave;
  leave.reason = "signing off";
  ASSERT_TRUE(stream->Send(ChatEvents::FromLeave(leave)).ok());
  auto left = stream->Receive();
  ASSERT_TRUE(left.ok() && left->has_value());
  ASSERT_TRUE((**left).is_left());
  EXPECT_EQ((**left).as_left().member, "grace");

  // Server-initiated close: the handler returned, so the next Receive is the
  // stream's natural end.
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatEndToEndTest, ServerPushStreamsWithoutAClientTransmitDirection) {
  WatchInput input;
  input.room = "lobby";
  auto stream = client_->Watch(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(last_dialed_target_, "/rooms/lobby/watch");

  for (int i = 0; i < 3; ++i) {
    auto update = stream->Receive();
    ASSERT_TRUE(update.ok() && update->has_value());
    ASSERT_TRUE((**update).is_message());
    EXPECT_EQ((**update).as_message().text, "lobby-update-" + std::to_string(i));
  }
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatEndToEndTest, UnaryOperationSharesTheService) {
  const auto rooms = client_->ListRooms();
  ASSERT_TRUE(rooms.ok()) << rooms.error().message();
  ASSERT_EQ(rooms->rooms.size(), 1U);
  EXPECT_EQ(rooms->rooms[0].name, "lobby");
  EXPECT_EQ(rooms->rooms[0].members, 2);
}

TEST_F(ChatEndToEndTest, ModeledMidStreamErrorSurfacesTypedOnTheClient) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "mallory";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  ChatMessage message;
  message.text = "kick me";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());

  // The exception message is terminal and unary-shaped: kind, code, message,
  // and the typed detail all match what a failed unary call would carry.
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "Kicked");
  EXPECT_EQ(outcome.error().message(), "kicked from lobby");
  const Kicked* detail = outcome.error().detail<Kicked>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->by, kModerator);

  // The generated error matcher dispatches it like any modeled error.
  const ConverseErrors errors = ConverseErrors::FromError(outcome.error());
  ASSERT_TRUE(errors.is_kicked());
  EXPECT_EQ(errors.as_kicked().by, kModerator);
}

TEST_F(ChatEndToEndTest, UnknownEventTypeIsATerminalSerializationError) {
  // Hold the far end raw: no serve thread, so the test can speak the
  // envelope convention (opal/eventstream/envelope.h) at the generated
  // client directly.
  serve_far_end_ = false;
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "eve";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_EQ(sessions_.size(), 1U);
  const std::shared_ptr<opal::http::WebSocket> far = sessions_.back();

  // A well-formed event message whose :event-type matches no RoomEvents
  // member — a newer peer's event, or a corrupted one.
  ASSERT_TRUE(far->Send(opal::eventstream::MakeEventMessage("presence", "application/json",
                                                            opal::Blob::FromString("{}")))
                  .ok());

  // Undecodable is terminal (ADR-0016): the error is Serialization-kinded
  // and names the stray type...
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kSerialization);
  EXPECT_NE(outcome.error().message().find("presence"), std::string::npos)
      << outcome.error().message();

  // ...and the receiving stream closed the session — the raw far end
  // observes the close, not a stalled stream.
  auto closed = far->Receive();
  ASSERT_TRUE(closed.ok()) << closed.error().message();
  EXPECT_FALSE(closed->has_value());
}

TEST_F(ChatEndToEndTest, GateRefusesAnUnknownStreamPath) {
  const auto gate = server_->StreamRouter()->Gate();
  opal::http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/nope";
  const auto refusal = gate(upgrade);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 404);

  // A routed upgrade is admitted (nullopt lets the transport upgrade).
  upgrade.target = "/rooms/lobby/converse";
  EXPECT_FALSE(gate(upgrade).has_value());
}

// ---------------------------------------------------------------------------
// DialStream's failure branches through the generated client.
// ---------------------------------------------------------------------------

TEST(ChatStreamDialTest, StreamingWithoutEndpointOrDialerIsAValidationError) {
  // A client wired for unary only (http_client, no endpoint, no dialer) has
  // nowhere to dial a stream; the generated branch must name the fix.
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.http_client = std::make_shared<opal::http::Loopback>();
  auto client = ChatClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  ConverseInput input;
  input.room = "lobby";
  auto stream = client->Converse(input);
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().kind(), opal::ErrorKind::kValidation);
  EXPECT_NE(stream.error().message().find("endpoint or a websocket_dialer"), std::string::npos)
      << stream.error().message();
}

TEST(ChatStreamDialTest, ADialerFailureSurfacesVerbatimFromTheStreamingOperation) {
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.http_client = std::make_shared<opal::http::Loopback>();
  config.websocket_dialer = [](const opal::http::WebSocketDialRequest&)
      -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
    return opal::Error::Transport("dial refused by probe");
  };
  auto client = ChatClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  auto stream = client->Watch([] {
    WatchInput input;
    input.room = "lobby";
    return input;
  }());
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().kind(), opal::ErrorKind::kTransport);
  EXPECT_EQ(stream.error().message(), "dial refused by probe");
}

// ---------------------------------------------------------------------------
// The generated error paths a well-behaved handler never exercises: the
// exception-message builder's non-Kicked arms and the encoder's refusals.
// ---------------------------------------------------------------------------

// Echoes messages; returns scripted handler errors on magic texts — the
// error-path complement of RoomHandler (room_handler.h).
class ErrorScriptHandler final : public ChatHandler {
 public:
  opal::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                           const opal::server::RequestContext&) override {
    return ListRoomsOutput{};
  }

  opal::Outcome<opal::Unit> Converse(const ConverseInput&, ConverseServerStream& stream,
                                     const opal::server::RequestContext&) override {
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) return opal::Unit{};
      if (!(**event).is_message()) continue;
      const std::string& text = (**event).as_message().text;
      if (text == "crash") return opal::Error::Transport("secret-internal-detail");
      if (text == "reject") return opal::Error::Validation("bad input");
      if (text == "roomfull") return opal::Error::Modeled("RoomFull", "room is full");
      ChatMessage echo;
      echo.text = text;
      if (!stream.Send(RoomEvents::FromMessage(echo)).ok()) return opal::Unit{};
    }
  }

  opal::Outcome<opal::Unit> Watch(const WatchInput&, WatchServerStream& stream,
                                  const opal::server::RequestContext&) override {
    ChatMessage message;
    message.text = "update";
    (void)stream.Send(RoomEvents::FromMessage(message));
    return opal::Unit{};
  }
};

// The same shared fixture, with each test injecting its handler.
class ScriptedChatTest : public StreamTestFixture {};

TEST_F(ScriptedChatTest, SendingAnEmptyUnionFailsValidationAndSparesTheSession) {
  // The generated encoder's tail: a union with no member engaged is refused
  // before anything touches the wire. (Its NoEvents sibling — Send on a
  // Watch stream — needs no test anymore: it stopped compiling when
  // event_stream.h made Send a static_assert on NoEvents directions.)
  StartWith(std::make_shared<ErrorScriptHandler>());
  ConverseInput input;
  input.room = "lobby";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  const auto refused = stream->Send(ChatEvents{});  // no member engaged
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), opal::ErrorKind::kValidation);
  EXPECT_NE(refused.error().message().find("no event member engaged"), std::string::npos)
      << refused.error().message();

  // The session was never touched: a real event still round-trips.
  ChatMessage message;
  message.text = "still-alive";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto echo = stream->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  ASSERT_TRUE((**echo).is_message());
  EXPECT_EQ((**echo).as_message().text, "still-alive");
  stream->Close();
}

TEST_F(ScriptedChatTest, AnUnexpectedHandlerErrorNeverLeaksItsDetail) {
  // The exception-message builder's never-leak default: an unexpected
  // handler failure (here Transport-kinded) reaches the client as a generic
  // InternalFailure — the internal detail must not cross the wire.
  StartWith(std::make_shared<ErrorScriptHandler>());
  ConverseInput input;
  input.room = "lobby";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  ChatMessage message;
  message.text = "crash";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().code(), "InternalFailure");
  EXPECT_EQ(outcome.error().message(), "internal failure");
  EXPECT_EQ(outcome.error().message().find("secret-internal-detail"), std::string::npos);
}

TEST_F(ScriptedChatTest, AHandlerValidationErrorMapsToSerializationException) {
  // The kValidation/kSerialization arm: the identity flips to
  // SerializationException and the message is preserved (it names the
  // caller's mistake, not server internals).
  StartWith(std::make_shared<ErrorScriptHandler>());
  ConverseInput input;
  input.room = "lobby";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  ChatMessage message;
  message.text = "reject";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().code(), "SerializationException");
  EXPECT_EQ(outcome.error().message(), "bad input");
}

TEST_F(ScriptedChatTest, AnUndeclaredModeledErrorFallsThroughAsGenericModeled) {
  // A modeled error outside the operation's declared list (RoomFull is not
  // in Converse's errors): the builder sends it without detail
  // serialization, and the client decoder's fallthrough keeps it generic —
  // kModeled with the claimed code, matched by no ConverseErrors member.
  StartWith(std::make_shared<ErrorScriptHandler>());
  ConverseInput input;
  input.room = "lobby";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  ChatMessage message;
  message.text = "roomfull";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "RoomFull");
  EXPECT_EQ(outcome.error().message(), "room is full");
  EXPECT_FALSE(ConverseErrors::FromError(outcome.error()).is_kicked());
}

// ---------------------------------------------------------------------------
// The generated ASYNC serve path (ADR-0021): the same wire, zero threads.
// ---------------------------------------------------------------------------

// The smallest coroutine session loop, plus the scripted failure modes the
// generated wrapper must frame: a typed Kicked on "kick me", a throw on
// "explode" (StreamTask's containment, end to end), an echo otherwise.
class EchoAsyncHandler final : public ChatAsyncHandler {
 public:
  opal::eventstream::StreamTask Converse(ConverseInput input,
                                         ConverseAsyncServerStream& stream) override {
    const std::string name = input.nickname.value_or("anonymous");
    (void)co_await stream.Send(RoomEvents::FromJoined(MemberJoined{.member = name}));
    while (true) {
      auto event = co_await stream.Receive();
      if (!event.ok() || !event->has_value()) co_return opal::Unit{};
      if ((*event)->is_leave()) {
        (void)co_await stream.Send(RoomEvents::FromLeft(MemberLeft{.member = name}));
        co_return opal::Unit{};
      }
      if (!(*event)->is_message()) continue;
      const ChatMessage& message = (*event)->as_message();
      if (message.text == "kick me") {
        Kicked kicked;
        kicked.message = "kicked from " + input.room;
        kicked.by = kModerator;
        auto refusal = opal::Error::Modeled("Kicked", *kicked.message);
        refusal.set_detail(std::move(kicked));
        co_return refusal;
      }
      if (message.text == "explode") {
        throw std::runtime_error("secret-internal-detail");
      }
      (void)co_await stream.Send(RoomEvents::FromMessage(message));
    }
  }

  opal::eventstream::StreamTask Watch(WatchInput input, WatchAsyncServerStream& stream) override {
    for (int i = 0; i < 3; ++i) {
      ChatMessage update;
      update.text = input.room + "-update-" + std::to_string(i);
      (void)co_await stream.Send(RoomEvents::FromMessage(update));
    }
    co_return opal::Unit{};
  }

  opal::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                           const opal::server::RequestContext&) override {
    ListRoomsOutput output;
    output.rooms.push_back(RoomSummary{.name = "lobby", .members = 2});
    return output;
  }
};

class AsyncChatEndToEndTest : public StreamTestFixture {
 protected:
  void SetUp() override { StartWithAsync(std::make_shared<EchoAsyncHandler>()); }
};

TEST_F(AsyncChatEndToEndTest, BidiEventsRoundTripWithZeroServeThreads) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(last_dialed_target_, "/rooms/lobby/converse");
  // The structural claim of the seam: the launch point returned and no
  // serve thread exists — the pair's completions drive the coroutine.
  EXPECT_TRUE(threads_.empty());

  auto joined = stream->Receive();
  ASSERT_TRUE(joined.ok() && joined->has_value());
  ASSERT_TRUE((**joined).is_joined());
  EXPECT_EQ((**joined).as_joined().member, "ada");

  for (int i = 0; i < 5; ++i) {
    ChatMessage message;
    message.text = "hello-" + std::to_string(i);
    message.sender = "ada";
    ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
    auto echo = stream->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_TRUE((**echo).is_message());
    EXPECT_EQ((**echo).as_message().text, "hello-" + std::to_string(i));
  }

  // A clean leave: the handler co_returns Unit and the generated wrapper
  // closes — the next Receive is the stream's natural end.
  LeaveNotice leave;
  leave.reason = "done";
  ASSERT_TRUE(stream->Send(ChatEvents::FromLeave(leave)).ok());
  auto left = stream->Receive();
  ASSERT_TRUE(left.ok() && left->has_value());
  ASSERT_TRUE((**left).is_left());
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(AsyncChatEndToEndTest, AsyncWatchPushesWithoutAClientTransmitDirection) {
  WatchInput input;
  input.room = "lobby";
  auto stream = client_->Watch(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  for (int i = 0; i < 3; ++i) {
    auto update = stream->Receive();
    ASSERT_TRUE(update.ok() && update->has_value());
    ASSERT_TRUE((**update).is_message());
    EXPECT_EQ((**update).as_message().text, "lobby-update-" + std::to_string(i));
  }
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(AsyncChatEndToEndTest, AFailedTaskOutcomeSurfacesTypedOnTheClient) {
  // The blocking contract's best convenience, restored by StreamTask: the
  // handler co_returns the modeled error, the generated wrapper frames the
  // typed exception, and the client sees the exact unary-shaped failure.
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "mallory";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  ChatMessage message;
  message.text = "kick me";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "Kicked");
  EXPECT_EQ(outcome.error().message(), "kicked from lobby");
  const Kicked* detail = outcome.error().detail<Kicked>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->by, kModerator);
  ASSERT_TRUE(ConverseErrors::FromError(outcome.error()).is_kicked());
}

TEST_F(AsyncChatEndToEndTest, AThrowingAsyncHandlerSurfacesInternalFailureNotTermination) {
  // StreamTask's containment through the whole generated stack: the
  // coroutine throws, the task completes with Error::Unknown, the wrapper
  // frames the never-leak InternalFailure — and the process lives.
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "bug";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  ChatMessage message;
  message.text = "explode";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().code(), "InternalFailure");
  EXPECT_EQ(outcome.error().message(), "internal failure");
  EXPECT_EQ(outcome.error().message().find("secret-internal-detail"), std::string::npos);
}

TEST_F(AsyncChatEndToEndTest, UnaryOperationSharesTheAsyncService) {
  const auto rooms = client_->ListRooms();
  ASSERT_TRUE(rooms.ok()) << rooms.error().message();
  ASSERT_EQ(rooms->rooms.size(), 1U);
  EXPECT_EQ(rooms->rooms[0].name, "lobby");
  EXPECT_EQ(rooms->rooms[0].members, 2);
}

// Fills the wire through a Share() handle, then refuses — the production
// shape (the hub's admission path mints handles before its Kicked), timed
// so the wrapper's exception send cannot complete inline.
class FloodThenRefuseHandler final : public ChatAsyncHandler {
 public:
  opal::eventstream::StreamTask Converse(ConverseInput input,
                                         ConverseAsyncServerStream& stream) override {
    auto handle = stream.Share();
    for (std::size_t i = 0; i < opal::http::InMemoryWebSocketPair::kQueueDepth; ++i) {
      ChatMessage fill;
      fill.text = "fill-" + std::to_string(i);
      handle.SendAsync(RoomEvents::FromMessage(fill), [](const opal::Outcome<opal::Unit>&) {});
    }
    Kicked kicked;
    kicked.message = "kicked from " + input.room;
    kicked.by = kModerator;
    auto refusal = opal::Error::Modeled("Kicked", *kicked.message);
    refusal.set_detail(std::move(kicked));
    co_return refusal;
  }

  opal::eventstream::StreamTask Watch(WatchInput, WatchAsyncServerStream& stream) override {
    (void)co_await stream.Receive();
    co_return opal::Unit{};
  }

  opal::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                           const opal::server::RequestContext&) override {
    return ListRoomsOutput{};
  }
};

class AsyncRefusalTest : public StreamTestFixture {};

TEST_F(AsyncRefusalTest, ATypedRefusalSurvivesAFullWire) {
  // The refusal's exception message parks on the full wire; the generated
  // wrapper must keep the session open (its frame, and the stream the
  // frame owns) until that send completes — a regression that closes
  // first cancels the parked write, and the client sees a bare close
  // instead of the modeled error.
  StartWithAsync(std::make_shared<FloodThenRefuseHandler>());
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  for (std::size_t i = 0; i < opal::http::InMemoryWebSocketPair::kQueueDepth; ++i) {
    auto fill = stream->Receive();
    ASSERT_TRUE(fill.ok() && fill->has_value()) << "fill " << i << " missing";
  }
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok()) << "the typed refusal was lost to the close";
  EXPECT_EQ(outcome.error().code(), "Kicked");
  EXPECT_EQ(outcome.error().message(), "kicked from lobby");
}

}  // namespace
}  // namespace example::chat
