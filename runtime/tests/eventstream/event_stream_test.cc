// Pins ADR-0016's typed session over the in-memory pair: EventStream
// plumbs its two codec functions onto the WebSocket contract — typed
// round-trips both ways, decode failures (received exceptions) terminal
// with a close, clean close as nullopt, and error passthrough that leaves
// encode failures non-fatal. The EventStreamHandle half (issue #112) pins
// the shared view's contract: handles send through the live stream from
// any thread, fail softly (Error::Transport, never a dangle) once the
// stream is gone, and the stream's destructor waits out handle operations
// in flight before the borrow dies.

#include "opal/eventstream/event_stream.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "opal/eventstream/frame.h"
#include "opal/http/websocket.h"
#include "opal/http/websocket_pair.h"

namespace opal::eventstream {
namespace {

// Distinct client-to-server and server-to-client types, so a swapped
// type parameter cannot compile away silently.
struct Ping {
  int number = 0;
};
struct Pong {
  std::string text;
};

Outcome<Message> EncodePing(const Ping& ping) {
  return Message{.headers = {{":event-type", "ping"}},
                 .payload = Blob::FromString(std::to_string(ping.number))};
}

Outcome<Ping> DecodePing(const Message& message) {
  const std::string* type = message.FindString(":event-type");
  if (type == nullptr || *type != "ping") {
    return Error::Serialization("not a ping");
  }
  return Ping{std::stoi(message.payload.ToString())};
}

Outcome<Message> EncodePong(const Pong& pong) {
  return Message{.headers = {{":event-type", "pong"}}, .payload = Blob::FromString(pong.text)};
}

// The generated-decoder shape: a modeled exception message decodes to the
// typed Error, everything else to the event.
Outcome<Pong> DecodePong(const Message& message) {
  if (const std::string* exception = message.FindString(":exception-type"); exception != nullptr) {
    return Error::Modeled(*exception, message.payload.ToString());
  }
  return Pong{message.payload.ToString()};
}

using ClientStream = EventStream<Ping, Pong>;
using ServerStream = EventStream<Pong, Ping>;

TEST(EventStreamTest, TypedEventsRoundTripBothDirections) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  ServerStream server(server_socket, EncodePong, DecodePing);

  ASSERT_TRUE(client.Send(Ping{41}).ok());
  auto ping = server.Receive();
  ASSERT_TRUE(ping.ok() && ping->has_value());
  EXPECT_EQ((*ping)->number, 41);

  ASSERT_TRUE(server.Send(Pong{"forty-two"}).ok());
  auto pong = client.Receive();
  ASSERT_TRUE(pong.ok() && pong->has_value());
  EXPECT_EQ((*pong)->text, "forty-two");
}

TEST(EventStreamTest, TheBorrowedSocketConstructorServesTheServerPath) {
  // on_websocket lends a WebSocket&; the stream borrows it for the serve
  // callback's lifetime.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  ServerStream server(*server_socket, EncodePong, DecodePing);

  ASSERT_TRUE(client.Send(Ping{7}).ok());
  auto ping = server.Receive();
  ASSERT_TRUE(ping.ok() && ping->has_value());
  EXPECT_EQ((*ping)->number, 7);
  ASSERT_TRUE(server.Send(Pong{"seven"}).ok());
  auto pong = client.Receive();
  ASSERT_TRUE(pong.ok() && pong->has_value());
  EXPECT_EQ((*pong)->text, "seven");
}

TEST(EventStreamTest, ADecodedExceptionEndsTheStreamWithItsError) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);

  // The peer sends a modeled exception message, then keeps listening.
  ASSERT_TRUE(server_socket
                  ->Send(Message{.headers = {{":exception-type", "RoomFull"}},
                                 .payload = Blob::FromString("room is full")})
                  .ok());

  // The decoder's error surfaces as the Receive outcome...
  const auto received = client.Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kModeled);
  EXPECT_EQ(received.error().code(), "RoomFull");
  EXPECT_EQ(received.error().message(), "room is full");

  // ...and the exception was terminal (ADR-0016): the stream closed the
  // session, so this end cannot send and the peer sees the clean close.
  const auto after = client.Send(Ping{1});
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.error().kind(), ErrorKind::kTransport);
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok());
  EXPECT_FALSE(at_peer->has_value());
}

TEST(EventStreamTest, AnUndecodableMessageIsEquallyTerminal) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ServerStream server(server_socket, EncodePong, DecodePing);

  ASSERT_TRUE(client_socket->Send(Message{.headers = {{":event-type", "not-a-ping"}}}).ok());
  const auto received = server.Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
  EXPECT_FALSE(server.Send(Pong{"x"}).ok());
}

TEST(EventStreamTest, ThePeersCleanCloseIsNullopt) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);

  ASSERT_TRUE(server_socket->Send(Message{.payload = Blob::FromString("last words")}).ok());
  server_socket->Close();

  // The message queued before the close still arrives...
  auto last = client.Receive();
  ASSERT_TRUE(last.ok() && last->has_value());
  EXPECT_EQ((*last)->text, "last words");
  // ...then the stream's natural end, sticky across calls.
  auto closed = client.Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
  auto again = client.Receive();
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again->has_value());
}

TEST(EventStreamTest, SendAfterCloseIsATransportError) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);

  client.Close();
  client.Close();  // idempotent, per the WebSocket contract
  const auto outcome = client.Send(Ping{1});
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);
}

TEST(EventStreamTest, AnEncoderFailureSurfacesWithoutEndingTheSession) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream failing(
      client_socket,
      [](const Ping& ping) -> Outcome<Message> {
        if (ping.number < 0) return Error::Validation("negative ping");
        return EncodePing(ping);
      },
      DecodePong);

  const auto refused = failing.Send(Ping{-1});
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  // The session was never touched: the next Send still goes through.
  ASSERT_TRUE(failing.Send(Ping{5}).ok());
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
  EXPECT_EQ((*at_peer)->payload.ToString(), "5");
}

// ---------------------------------------------------------------------------
// EventStream::Share and EventStreamHandle (issue #112): the shared view.
// ---------------------------------------------------------------------------

TEST(EventStreamHandleTest, HandleSendsThroughTheLiveStream) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  const auto handle = client.Share();

  // Handle sends and stream sends interleave on one session, in order.
  ASSERT_TRUE(handle.Send(Ping{1}).ok());
  ASSERT_TRUE(client.Send(Ping{2}).ok());
  ASSERT_TRUE(handle.Send(Ping{3}).ok());
  for (int expected = 1; expected <= 3; ++expected) {
    auto at_peer = server_socket->Receive();
    ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
    EXPECT_EQ((*at_peer)->payload.ToString(), std::to_string(expected));
  }
}

TEST(EventStreamHandleTest, HandleOutlivesTheStreamAndFailsSoftly) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::optional<EventStreamHandle<Ping>> handle;
  {
    ClientStream client(client_socket, EncodePing, DecodePong);
    handle = client.Share();
    ASSERT_TRUE(handle->Send(Ping{1}).ok());
  }  // the borrow ends here — the handler-returns moment

  // Destroying a Share()d stream ends the session: the peer sees the close
  // (after draining what was sent) rather than a stall.
  ASSERT_TRUE(server_socket->Receive().ok());
  auto closed = server_socket->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());

  // The stale handle is safe forever: Send fails like any closed stream,
  // Close is a no-op — no dangling reference, no new failure mode.
  const auto after = handle->Send(Ping{2});
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.error().kind(), ErrorKind::kTransport);
  handle->Close();
  EXPECT_FALSE(handle->Send(Ping{3}).ok());
}

TEST(EventStreamHandleTest, EveryHandleSharesOneRevocableView) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::optional<EventStreamHandle<Ping>> first;
  std::optional<EventStreamHandle<Ping>> second;
  {
    ClientStream client(client_socket, EncodePing, DecodePong);
    first = client.Share();
    second = client.Share();
    ASSERT_TRUE(first->Send(Ping{1}).ok());
    ASSERT_TRUE(second->Send(Ping{2}).ok());
  }
  EXPECT_FALSE(first->Send(Ping{3}).ok());
  EXPECT_FALSE(second->Send(Ping{3}).ok());
}

TEST(EventStreamHandleTest, HandleWorksOnTheBorrowedServerPath) {
  // The issue-#112 shape: the server path borrows a WebSocket&, and the
  // handle must outlive that borrow safely.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::optional<EventStreamHandle<Pong>> handle;
  {
    ServerStream server(*server_socket, EncodePong, DecodePing);
    handle = server.Share();
    ASSERT_TRUE(handle->Send(Pong{"from-the-hub"}).ok());
  }
  auto at_client = client_socket->Receive();
  ASSERT_TRUE(at_client.ok() && at_client->has_value());
  EXPECT_EQ((*at_client)->payload.ToString(), "from-the-hub");
  const auto stale = handle->Send(Pong{"late"});
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().kind(), ErrorKind::kTransport);
}

TEST(EventStreamHandleTest, HandleCloseEndsTheSessionForTheStreamOwner) {
  // The hub's kick path: Close from a thread that only holds the handle
  // unblocks the owner's Receive with the stream's natural end.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  const auto handle = client.Share();

  std::thread closer([&handle] { handle.Close(); });
  auto end = client.Receive();
  closer.join();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

TEST(EventStreamHandleTest, HandleSendAfterSessionCloseIsATransportError) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  const auto handle = client.Share();
  client.Close();

  // The stream still exists; the session is what died. Same error shape.
  const auto outcome = handle.Send(Ping{1});
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);
}

TEST(EventStreamHandleTest, AnEncoderFailureThroughTheHandleSparesTheSession) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream failing(
      client_socket,
      [](const Ping& ping) -> Outcome<Message> {
        if (ping.number < 0) return Error::Validation("negative ping");
        return EncodePing(ping);
      },
      DecodePong);
  const auto handle = failing.Share();

  const auto refused = handle.Send(Ping{-1});
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  ASSERT_TRUE(handle.Send(Ping{5}).ok());
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
  EXPECT_EQ((*at_peer)->payload.ToString(), "5");
}

TEST(EventStreamHandleTest, DestructionWaitsOutABlockedHandleSend) {
  // The race the shared view exists to make safe: a handle Send is blocked
  // on backpressure (the peer stopped reading) when the stream dies. The
  // destructor must close the session (failing the blocked Send), wait for
  // it to leave the socket, and only then let the borrow go.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::atomic<bool> send_returned{false};
  Outcome<Unit> blocked_outcome = Unit{};
  std::thread sender;
  {
    ClientStream client(client_socket, EncodePing, DecodePong);
    const auto handle = client.Share();
    // Fill the pair's send bound so the next Send blocks on the wire.
    for (std::size_t i = 0; i < http::InMemoryWebSocketPair::kQueueDepth; ++i) {
      ASSERT_TRUE(client.Send(Ping{static_cast<int>(i)}).ok());
    }
    sender = std::thread([handle, &send_returned, &blocked_outcome] {
      blocked_outcome = handle.Send(Ping{99});
      send_returned = true;
    });
    // Bias the race toward the interesting interleaving (sender blocked
    // inside the socket call when the stream dies); the other interleaving
    // (revoked before Acquire) satisfies the same assertions.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }  // ~EventStream: close, drain the in-flight Send, revoke

  sender.join();
  EXPECT_TRUE(send_returned);
  ASSERT_FALSE(blocked_outcome.ok());
  EXPECT_EQ(blocked_outcome.error().kind(), ErrorKind::kTransport);
}

TEST(EventStreamHandleTest, TheSharedViewSurvivesAMove) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::optional<EventStreamHandle<Ping>> handle;
  {
    ClientStream original(client_socket, EncodePing, DecodePong);
    handle = original.Share();
    ClientStream moved(std::move(original));
    ASSERT_TRUE(handle->Send(Ping{7}).ok());
    auto at_peer = server_socket->Receive();
    ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
    EXPECT_EQ((*at_peer)->payload.ToString(), "7");
  }  // the moved-to stream owns the view and ends it
  EXPECT_FALSE(handle->Send(Ping{8}).ok());
}

TEST(EventStreamTest, ATimedReceiveExpiresTypedAndLeavesTheStreamUsable) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  ServerStream server(server_socket, EncodePong, DecodePing);

  // The event the peer never sends: bounded, so the caller gets a verdict
  // instead of a hang.
  auto nothing = client.Receive(std::chrono::milliseconds(50));
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError");

  // Unlike a decode failure, the timeout closed nothing: the session picks
  // up where it left off, both ways.
  ASSERT_TRUE(server.Send(Pong{"eventually"}).ok());
  auto pong = client.Receive(std::chrono::seconds(5));
  ASSERT_TRUE(pong.ok() && pong->has_value());
  EXPECT_EQ((*pong)->text, "eventually");
  ASSERT_TRUE(client.Send(Ping{9}).ok());
  auto ping = server.Receive(std::chrono::seconds(5));
  ASSERT_TRUE(ping.ok() && ping->has_value());
  EXPECT_EQ((*ping)->number, 9);
}

TEST(EventStreamTest, ATimedReceiveStillReportsTheCleanCloseAndTerminalDecodes) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  {
    ClientStream client(client_socket, EncodePing, DecodePong);
    server_socket->Close();
    auto end = client.Receive(std::chrono::seconds(5));
    ASSERT_TRUE(end.ok());
    EXPECT_FALSE(end->has_value());  // the close, not the deadline
  }
  // A decoder failure stays terminal under a deadline: the stream closes
  // and the error surfaces, exactly as the untimed overload does.
  auto [client_two, server_two] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_two, EncodePing, DecodePong);
  ASSERT_TRUE(server_two
                  ->Send(Message{.headers = {{":exception-type", "RoomFull"}},
                                 .payload = Blob::FromString("room is full")})
                  .ok());
  auto exception = client.Receive(std::chrono::seconds(5));
  ASSERT_FALSE(exception.ok());
  EXPECT_EQ(exception.error().code(), "RoomFull");
  EXPECT_FALSE(server_two->Send(Message{}).ok());  // the stream closed the session
}

TEST(EventStreamHandleTest, AReceiveOnlyStreamStillSharesForClose) {
  // A NoEvents transmit direction has nothing to Send (compile-enforced),
  // but a hub still wants Close on watchers.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  EventStream<NoEvents, Pong> watcher(client_socket, nullptr, DecodePong);
  const auto handle = watcher.Share();
  handle.Close();
  auto end = watcher.Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

}  // namespace
}  // namespace opal::eventstream
