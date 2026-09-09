// Pins the in-memory WebSocket pair (ADR-0016) to the WebSocket contract
// the Beast sessions implement: blocking round-trips both ways, full-duplex
// concurrency, bounded-queue backpressure, and close semantics — clean
// nullopt after draining, Error::Transport on Send, idempotence, and
// unblocking blocked calls from another thread — plus the bounded receive
// (Error::Timeout on a deadline that expires, on a session that survives it).

#include "smithy/http/websocket_pair.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace opal::http {
namespace {

using eventstream::Message;

Message Text(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = Blob::FromString(body)};
}

// Blocks until check() holds or the deadline passes; the polling half of
// timing-sensitive assertions (the sleeping half asserts stall).
bool WaitFor(const std::function<bool()>& check) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!check()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(WebSocketPairTest, MessagesRoundTripBothWays) {
  auto [a, b] = InMemoryWebSocketPair::Create();

  ASSERT_TRUE(a->Send(Text("chat", "from a")).ok());
  auto at_b = b->Receive();
  ASSERT_TRUE(at_b.ok() && at_b->has_value());
  EXPECT_EQ(**at_b, Text("chat", "from a"));

  ASSERT_TRUE(b->Send(Text("chat", "from b")).ok());
  auto at_a = a->Receive();
  ASSERT_TRUE(at_a.ok() && at_a->has_value());
  EXPECT_EQ(**at_a, Text("chat", "from b"));
}

TEST(WebSocketPairTest, FullDuplexPingPongAcrossTwoThreads) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr int kRounds = 100;

  std::thread peer([&b] {
    for (int i = 0; i < kRounds; ++i) {
      auto message = b->Receive();
      ASSERT_TRUE(message.ok() && message->has_value()) << "round " << i;
      Message reply = **message;
      reply.payload = Blob::FromString("echo:" + (*message)->payload.ToString());
      ASSERT_TRUE(b->Send(reply).ok()) << "round " << i;
    }
  });

  for (int i = 0; i < kRounds; ++i) {
    ASSERT_TRUE(a->Send(Text("ping", std::to_string(i))).ok()) << "round " << i;
    auto reply = a->Receive();
    ASSERT_TRUE(reply.ok() && reply->has_value()) << "round " << i;
    EXPECT_EQ((*reply)->payload.ToString(), "echo:" + std::to_string(i)) << "round " << i;
  }
  peer.join();
}

TEST(WebSocketPairTest, BothDirectionsStreamConcurrently) {
  // Two senders pumping opposite directions at once, far past the queue
  // bound — progress requires the two directions not to block each other.
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr int kCount = 64;

  std::thread from_a([&a] {
    for (int i = 0; i < kCount; ++i) {
      ASSERT_TRUE(a->Send(Text("a", std::to_string(i))).ok()) << "message " << i;
    }
  });
  std::thread from_b([&b] {
    for (int i = 0; i < kCount; ++i) {
      ASSERT_TRUE(b->Send(Text("b", std::to_string(i))).ok()) << "message " << i;
    }
  });

  // Each direction arrives complete and in order.
  for (int i = 0; i < kCount; ++i) {
    auto at_b = b->Receive();
    ASSERT_TRUE(at_b.ok() && at_b->has_value()) << "message " << i;
    EXPECT_EQ((*at_b)->payload.ToString(), std::to_string(i)) << "message " << i;
  }
  for (int i = 0; i < kCount; ++i) {
    auto at_a = a->Receive();
    ASSERT_TRUE(at_a.ok() && at_a->has_value()) << "message " << i;
    EXPECT_EQ((*at_a)->payload.ToString(), std::to_string(i)) << "message " << i;
  }
  from_a.join();
  from_b.join();
}

TEST(WebSocketPairTest, AFullQueueBlocksTheSenderUntilTheReceiverDrains) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr std::size_t kDepth = 8;  // the documented Beast-parity bound
  constexpr std::size_t kTotal = kDepth + 4;

  std::atomic<std::size_t> sent{0};
  std::thread sender([&a, &sent] {
    for (std::size_t i = 0; i < kTotal; ++i) {
      ASSERT_TRUE(a->Send(Text("n", std::to_string(i))).ok()) << "message " << i;
      sent.fetch_add(1);
    }
  });

  // The sender reaches the bound and stalls there: no amount of waiting
  // lets message kDepth+1 through while nothing drains.
  ASSERT_TRUE(WaitFor([&sent] { return sent.load() == kDepth; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(sent.load(), kDepth);

  // Each Receive frees exactly one slot.
  auto first = b->Receive();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((*first)->payload.ToString(), "0");
  ASSERT_TRUE(WaitFor([&sent] { return sent.load() == kDepth + 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(sent.load(), kDepth + 1);

  // Draining the rest releases the sender completely, in order.
  for (std::size_t i = 1; i < kTotal; ++i) {
    auto message = b->Receive();
    ASSERT_TRUE(message.ok() && message->has_value()) << "message " << i;
    EXPECT_EQ((*message)->payload.ToString(), std::to_string(i)) << "message " << i;
  }
  sender.join();
  EXPECT_EQ(sent.load(), kTotal);
}

TEST(WebSocketPairTest, QueuedMessagesDrainBeforeTheCloseIsReported) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  ASSERT_TRUE(a->Send(Text("n", "1")).ok());
  ASSERT_TRUE(a->Send(Text("n", "2")).ok());
  a->Close();

  // The peer still owns what arrived before the close, in order...
  auto first = b->Receive();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((*first)->payload.ToString(), "1");
  auto second = b->Receive();
  ASSERT_TRUE(second.ok() && second->has_value());
  EXPECT_EQ((*second)->payload.ToString(), "2");
  // ...then the clean close, sticky across repeated calls.
  auto closed = b->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
  auto again = b->Receive();
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again->has_value());
}

TEST(WebSocketPairTest, SendFailsWithTransportOnceEitherEndCloses) {
  {
    auto [a, b] = InMemoryWebSocketPair::Create();
    a->Close();
    const auto own = a->Send(Text("n", "x"));
    ASSERT_FALSE(own.ok());
    EXPECT_EQ(own.error().kind(), ErrorKind::kTransport);
    const auto peer = b->Send(Text("n", "x"));
    ASSERT_FALSE(peer.ok());
    EXPECT_EQ(peer.error().kind(), ErrorKind::kTransport);
  }
  {
    // Symmetric from the other end.
    auto [a, b] = InMemoryWebSocketPair::Create();
    b->Close();
    EXPECT_FALSE(a->Send(Text("n", "x")).ok());
    EXPECT_FALSE(b->Send(Text("n", "x")).ok());
  }
}

TEST(WebSocketPairTest, CloseIsIdempotentFromBothEnds) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  a->Close();
  a->Close();
  b->Close();
  b->Close();
  auto at_a = a->Receive();
  ASSERT_TRUE(at_a.ok());
  EXPECT_FALSE(at_a->has_value());
  auto at_b = b->Receive();
  ASSERT_TRUE(at_b.ok());
  EXPECT_FALSE(at_b->has_value());
}

TEST(WebSocketPairTest, CloseUnblocksABlockedReceive) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  std::atomic<bool> unblocked{false};
  std::thread receiver([&b, &unblocked] {
    auto message = b->Receive();  // blocks: nothing was sent
    EXPECT_TRUE(message.ok());
    EXPECT_FALSE(message->has_value());
    unblocked.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(unblocked.load());
  a->Close();
  receiver.join();
  EXPECT_TRUE(unblocked.load());
}

TEST(WebSocketPairTest, CloseUnblocksASenderStalledOnBackpressure) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr std::size_t kDepth = 8;
  for (std::size_t i = 0; i < kDepth; ++i) {
    ASSERT_TRUE(a->Send(Text("n", std::to_string(i))).ok());
  }
  std::atomic<bool> failed{false};
  std::thread sender([&a, &failed] {
    const auto outcome = a->Send(Text("n", "overflow"));  // blocks on the full queue
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);
    failed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(failed.load());
  b->Close();
  sender.join();
  EXPECT_TRUE(failed.load());
}

TEST(WebSocketPairTest, AMessageTheCodecRefusesFailsValidationAndSparesTheSession) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  const auto refused = a->Send(Message{.headers = {{"", true}}});  // empty header name
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  // Wire-transport parity: the session is untouched and stays usable.
  ASSERT_TRUE(a->Send(Text("chat", "still alive")).ok());
  auto message = b->Receive();
  ASSERT_TRUE(message.ok() && message->has_value());
  EXPECT_EQ((*message)->payload.ToString(), "still alive");
}

TEST(WebSocketPairTimeoutTest, AReceiveDeadlineExpiresWithTimeoutAndSparesTheSession) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  const auto started = std::chrono::steady_clock::now();
  auto nothing = b->Receive(std::chrono::milliseconds(50));
  const auto waited = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(nothing.ok());
  // The code is what separates "nothing yet" from a broken wire; both are
  // transport-kind failures.
  EXPECT_EQ(nothing.error().code(), "TimeoutError");
  EXPECT_EQ(nothing.error().kind(), ErrorKind::kTransport);
  // It really waited — an insta-Timeout on the blocking path would pass
  // every other assertion here. The floor is loose because the wait's clock
  // and this measurement's are not guaranteed to be the same one (libc++
  // times condition_variable::wait_for against the system clock), so a
  // skew between them must not turn into a flake.
  EXPECT_GE(waited, std::chrono::milliseconds(40));

  // The point of the deadline over Close(): the session is still live, in
  // both directions.
  ASSERT_TRUE(a->Send(Text("chat", "late")).ok());
  auto arrived = b->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(arrived.ok() && arrived->has_value());
  EXPECT_EQ((*arrived)->payload.ToString(), "late");
  ASSERT_TRUE(b->Send(Text("chat", "reply")).ok());
  auto at_a = a->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(at_a.ok() && at_a->has_value());
  EXPECT_EQ((*at_a)->payload.ToString(), "reply");
}

TEST(WebSocketPairTimeoutTest, AQueuedMessageBeatsTheDeadlineWithoutWaiting) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  ASSERT_TRUE(a->Send(Text("chat", "already here")).ok());
  // A non-positive timeout polls: what is in hand comes back, nothing
  // blocks.
  auto message = b->Receive(std::chrono::milliseconds(0));
  ASSERT_TRUE(message.ok() && message->has_value());
  EXPECT_EQ((*message)->payload.ToString(), "already here");
  // The queue is empty now, so the same poll times out immediately.
  auto empty = b->Receive(std::chrono::milliseconds(0));
  ASSERT_FALSE(empty.ok());
  EXPECT_EQ(empty.error().code(), "TimeoutError");
}

TEST(WebSocketPairTimeoutTest, ACleanCloseStillReadsAsNulloptNotATimeout) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  ASSERT_TRUE(a->Send(Text("chat", "last words")).ok());
  a->Close();
  // Queued messages drain first, then the stream's end — the deadline
  // changes neither, which is why a timeout needs its own outcome.
  auto drained = b->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(drained.ok() && drained->has_value());
  EXPECT_EQ((*drained)->payload.ToString(), "last words");
  auto end = b->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

TEST(WebSocketPairTimeoutTest, ADeadlineUnblocksOnAnArrivalWithoutBurningIt) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  std::thread sender([&a] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(a->Send(Text("chat", "on its way")).ok());
  });
  const auto started = std::chrono::steady_clock::now();
  auto message = b->Receive(std::chrono::seconds(5));
  const auto waited = std::chrono::steady_clock::now() - started;
  sender.join();
  ASSERT_TRUE(message.ok() && message->has_value());
  EXPECT_EQ((*message)->payload.ToString(), "on its way");
  EXPECT_LT(waited, std::chrono::seconds(5));
}

TEST(WebSocketPairTimeoutTest, ATimedOutReceiveReleasesTheOneOutstandingSlot) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  auto nothing = b->Receive(std::chrono::milliseconds(20));
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError");

  // The slot the timed-out receive held is free: the async twin arms
  // instead of refusing with "a receive is already outstanding".
  std::atomic<bool> delivered{false};
  b->ReceiveAsync([&delivered](Outcome<std::optional<Message>> message) {
    EXPECT_TRUE(message.ok() && message->has_value());
    delivered.store(true);
  });
  ASSERT_TRUE(a->Send(Text("chat", "for the parked receive")).ok());
  EXPECT_TRUE(WaitFor([&delivered] { return delivered.load(); }));
}

TEST(WebSocketPairTimeoutTest, EveryImplementorAnswersTheDeadlineItself) {
  // The deadline is pure virtual, so there is no default to be wrong
  // about: a WebSocket implementation — a consumer's test fake, an adapter
  // over another library — answers it or does not compile. This is the
  // minimal shape, and what a fake owes its callers: honor the wait, and
  // report Error::Timeout rather than an unbounded block or a close.
  class Fake final : public WebSocket {
   public:
    Outcome<std::optional<Message>> Receive() override { return std::optional<Message>(); }
    Outcome<std::optional<Message>> Receive(std::chrono::milliseconds timeout) override {
      last_timeout = timeout;
      return Error::Timeout("fake: nothing to deliver");
    }
    Outcome<Unit> Send(const Message&) override { return Unit{}; }
    void Close() override {}
    std::chrono::milliseconds last_timeout{0};
  };
  Fake fake;
  // Reachable both on the concrete type and through the base reference
  // every layer above holds a session by.
  auto direct = fake.Receive(std::chrono::milliseconds(7));
  ASSERT_FALSE(direct.ok());
  EXPECT_EQ(direct.error().code(), "TimeoutError");
  EXPECT_EQ(fake.last_timeout, std::chrono::milliseconds(7));
  WebSocket& base = fake;
  EXPECT_TRUE(base.Receive().ok());
  EXPECT_FALSE(base.Receive(std::chrono::milliseconds(1)).ok());
}

}  // namespace
}  // namespace opal::http
