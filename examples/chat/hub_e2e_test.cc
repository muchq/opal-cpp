// The consumer-hub e2e, in-memory half (issue #112): several generated
// ChatClients against one HubHandler through injected InMemoryWebSocketPair
// dialers — fan-out with per-viewer redaction, watchers on the same
// registry, room isolation, the typed nickname refusal, departures both
// clean and abrupt, the slow-consumer disconnect, and Drain() emptying the
// hub. hub_cli_test.sh is the real-process half: the same handler behind
// BeastServerTransport, driven by shell commands.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example/chat/client.h"
#include "example/chat/server.h"
#include "hub_handler.h"
#include "smithy/client/config.h"
#include "smithy/http/loopback.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"
#include "stream_test_fixture.h"

namespace example::chat {
namespace {

// One received room event, or nullopt on the stream's end — for both stream
// flavors (converse and watch receive the same RoomEvents).
template <typename Stream>
std::optional<RoomEvents> Next(Stream& stream) {
  auto event = stream.Receive();
  if (!event.ok() || !event->has_value()) return std::nullopt;
  return **event;
}

class HubEndToEndTest : public StreamTestFixture {
 protected:
  void SetUp() override { StartHub({}); }

  // The shared fixture (stream_test_fixture.h) around a fresh HubHandler.
  void StartHub(HubHandler::Registry::Options options) {
    handler_ = std::make_shared<HubHandler>(std::move(options));
    StartWith(handler_);
  }

  // Sessions register a beat after their dial returns (the serve thread
  // races the caller), so tests wait for the hub's count before acting on
  // membership.
  void WaitForSessions(std::size_t count) {
    for (int i = 0; i < 100 && handler_->sessions() < count; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(handler_->sessions(), count);
  }

  // Dials Converse for name into room and drains the join echo every member
  // receives about itself.
  ConverseClientStream Join(const std::string& room, const std::string& name) {
    ConverseInput input;
    input.room = room;
    input.nickname = name;
    auto stream = client_->Converse(input);
    EXPECT_TRUE(stream.ok()) << stream.error().message();
    const auto joined = Next(*stream);
    EXPECT_TRUE(joined.has_value() && joined->is_joined());
    EXPECT_EQ(joined->as_joined().member, name);
    return std::move(*stream);
  }

  static ChatEvents Say(const std::string& text) {
    ChatMessage message;
    message.text = text;
    return ChatEvents::FromMessage(message);
  }

  std::shared_ptr<HubHandler> handler_;
};

TEST_F(HubEndToEndTest, MessagesFanOutWithPerViewerRedaction) {
  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream grace = Join("lobby", "grace");

  // ada sees grace arrive (the fan-out already crossing sessions).
  const auto arrival = Next(ada);
  ASSERT_TRUE(arrival.has_value() && arrival->is_joined());
  EXPECT_EQ(arrival->as_joined().member, "grace");

  // One message, two renderings: the author reads "you", the room reads the
  // author's name — Broadcast's per-recipient construction end to end.
  ASSERT_TRUE(ada.Send(Say("hello everyone")).ok());
  const auto at_ada = Next(ada);
  ASSERT_TRUE(at_ada.has_value() && at_ada->is_message());
  EXPECT_EQ(at_ada->as_message().text, "hello everyone");
  EXPECT_EQ(at_ada->as_message().sender, "you");
  const auto at_grace = Next(grace);
  ASSERT_TRUE(at_grace.has_value() && at_grace->is_message());
  EXPECT_EQ(at_grace->as_message().text, "hello everyone");
  EXPECT_EQ(at_grace->as_message().sender, "ada");

  ASSERT_TRUE(grace.Send(Say("hi ada")).ok());
  const auto reply_at_ada = Next(ada);
  ASSERT_TRUE(reply_at_ada.has_value() && reply_at_ada->is_message());
  EXPECT_EQ(reply_at_ada->as_message().sender, "grace");
  const auto reply_at_grace = Next(grace);
  ASSERT_TRUE(reply_at_grace.has_value() && reply_at_grace->is_message());
  EXPECT_EQ(reply_at_grace->as_message().sender, "you");

  ada.Close();
  grace.Close();
}

TEST_F(HubEndToEndTest, AWatcherObservesTheRoomThroughTheSameRegistry) {
  ConverseClientStream ada = Join("lobby", "ada");
  WatchInput input;
  input.room = "lobby";
  auto watcher = client_->Watch(input);
  ASSERT_TRUE(watcher.ok()) << watcher.error().message();

  WaitForSessions(2);

  ASSERT_TRUE(ada.Send(Say("for the record")).ok());
  const auto observed = Next(*watcher);
  ASSERT_TRUE(observed.has_value() && observed->is_message());
  EXPECT_EQ(observed->as_message().text, "for the record");
  EXPECT_EQ(observed->as_message().sender, "ada");  // watchers are not "you"

  ada.Close();
  watcher->Close();
}

TEST_F(HubEndToEndTest, RoomsDoNotCrossTalk) {
  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream bob = Join("ops", "bob");

  ASSERT_TRUE(ada.Send(Say("lobby only")).ok());
  ASSERT_TRUE(Next(ada).has_value());  // ada's own echo — the fan-out ran

  // FIFO makes absence provable: bob's next event after his own send must
  // be that send's echo, so nothing from the lobby ever reached his room.
  ASSERT_TRUE(bob.Send(Say("ops only")).ok());
  const auto at_bob = Next(bob);
  ASSERT_TRUE(at_bob.has_value() && at_bob->is_message());
  EXPECT_EQ(at_bob->as_message().text, "ops only");
  EXPECT_EQ(at_bob->as_message().sender, "you");

  ada.Close();
  bob.Close();
}

TEST_F(HubEndToEndTest, ANicknameCollisionIsRefusedWithTheTypedError) {
  ConverseClientStream ada = Join("lobby", "ada");

  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto impostor = client_->Converse(input);
  ASSERT_TRUE(impostor.ok()) << impostor.error().message();
  auto outcome = impostor->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "Kicked");
  const Kicked* detail = outcome.error().detail<Kicked>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->by, "hub");

  // The original session was untouched by the refusal.
  ASSERT_TRUE(ada.Send(Say("still me")).ok());
  const auto echo = Next(ada);
  ASSERT_TRUE(echo.has_value() && echo->is_message());
  EXPECT_EQ(echo->as_message().sender, "you");
  ada.Close();
}

TEST_F(HubEndToEndTest, TheUnaryNeighborReportsLiveOccupancy) {
  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream grace = Join("lobby", "grace");
  ConverseClientStream bob = Join("ops", "bob");
  ASSERT_TRUE(Next(ada).has_value());  // grace's arrival — lobby fully joined

  const auto rooms = client_->ListRooms();
  ASSERT_TRUE(rooms.ok()) << rooms.error().message();
  ASSERT_EQ(rooms->rooms.size(), 2U);  // map order: lobby, ops
  EXPECT_EQ(rooms->rooms[0].name, "lobby");
  EXPECT_EQ(rooms->rooms[0].members, 2);
  EXPECT_EQ(rooms->rooms[1].name, "ops");
  EXPECT_EQ(rooms->rooms[1].members, 1);

  ada.Close();
  grace.Close();
  bob.Close();
}

TEST_F(HubEndToEndTest, ACleanLeaveEchoesBackThenCloses) {
  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream grace = Join("lobby", "grace");
  ASSERT_TRUE(Next(ada).has_value());  // grace's arrival

  ASSERT_TRUE(grace.Send(ChatEvents::FromLeave(LeaveNotice{.reason = "signing off"})).ok());
  const auto goodbye = Next(grace);
  ASSERT_TRUE(goodbye.has_value() && goodbye->is_left());
  EXPECT_EQ(goodbye->as_left().member, "grace");
  EXPECT_FALSE(Next(grace).has_value());  // then the server's clean close

  const auto at_ada = Next(ada);
  ASSERT_TRUE(at_ada.has_value() && at_ada->is_left());
  EXPECT_EQ(at_ada->as_left().member, "grace");
  ada.Close();
}

TEST_F(HubEndToEndTest, AnAbruptDisconnectStillAnnouncesTheDeparture) {
  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream grace = Join("lobby", "grace");
  ASSERT_TRUE(Next(ada).has_value());  // grace's arrival

  grace.Close();  // no LeaveNotice — the vanish path

  const auto at_ada = Next(ada);
  ASSERT_TRUE(at_ada.has_value() && at_ada->is_left());
  EXPECT_EQ(at_ada->as_left().member, "grace");

  // The id is free again: the nickname reservation died with the session.
  ConverseClientStream regrace = Join("lobby", "grace");
  regrace.Close();
  ada.Close();
}

TEST_F(HubEndToEndTest, ASlowConsumerIsDisconnectedNotWaitedFor) {
  // Tiny queues so the sloth trips the policy quickly; ada must never feel
  // it. This is the hub's answer to the slowest client's TCP window.
  HubHandler::Registry::Options options;
  options.queue_capacity = 2;
  StartHub(std::move(options));

  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream sloth = Join("lobby", "sloth");
  ASSERT_TRUE(Next(ada).has_value());  // sloth's arrival

  // ada floods; sloth never reads. Reading each round's own echo asserts
  // liveness and keeps ada's wire drained; the hub's left-announcement for
  // the disconnected sloth interleaves whenever the policy fires.
  constexpr int kFlood = 5 * opal::http::InMemoryWebSocketPair::kQueueDepth;
  bool sloth_left = false;
  for (int i = 0; i < kFlood; ++i) {
    ASSERT_TRUE(ada.Send(Say("tick-" + std::to_string(i))).ok());
    for (;;) {
      const auto event = Next(ada);
      ASSERT_TRUE(event.has_value()) << "stalled at tick " << i;
      if (event->is_left()) {
        EXPECT_EQ(event->as_left().member, "sloth");
        sloth_left = true;
        continue;
      }
      ASSERT_TRUE(event->is_message());
      EXPECT_EQ(event->as_message().sender, "you");
      break;
    }
  }

  // The sloth's session was closed mid-flood: it drains a strict prefix,
  // then sees the stream end instead of a stall.
  int delivered = 0;
  while (Next(sloth).has_value()) ++delivered;
  EXPECT_LT(delivered, kFlood);

  // And the hub announced the disconnect like any other departure.
  while (!sloth_left) {
    const auto event = Next(ada);
    ASSERT_TRUE(event.has_value());
    sloth_left = event->is_left() && event->as_left().member == "sloth";
  }
  ada.Close();
}

TEST_F(HubEndToEndTest, DrainClosesEverySessionAndEmptiesTheHub) {
  ConverseClientStream ada = Join("lobby", "ada");
  ConverseClientStream grace = Join("lobby", "grace");
  WatchInput input;
  input.room = "lobby";
  auto watcher = client_->Watch(input);
  ASSERT_TRUE(watcher.ok()) << watcher.error().message();
  WaitForSessions(3);

  // Proposal 3, end to end: every handler wakes, unwinds, deregisters.
  EXPECT_TRUE(handler_->Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(handler_->sessions(), 0U);

  // Each client observes an orderly end (possibly after departure events
  // the fan-out queued while draining), never a stall.
  while (Next(ada).has_value()) {
  }
  while (Next(grace).has_value()) {
  }
  while (Next(*watcher).has_value()) {
  }
}

}  // namespace
}  // namespace example::chat
