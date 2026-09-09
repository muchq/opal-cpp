// Pins the SessionRegistry contract (issue #112): owning handles in, queued
// non-blocking fan-out out. The load-bearing behaviors: SendTo/Broadcast
// never block on a slow client's wire, per-recipient event construction,
// the slow-consumer policy (close-on-full by default, callback override),
// Remove-without-close bookkeeping, CloseAll/Drain as the graceful-shutdown
// step, and a destructor that always joins its writers — plus the stale-
// handle case the shared view exists for: a session whose stream died stays
// registered without dangling.
//
// The grace matrix (ADR-0020, issue #116): Detach parks, Resume swaps in
// the new connection (waiting out a wedged writer or a parked chain and
// joining what it displaced), expiry runs on_expired exactly once mutually
// exclusive with Resume — raced right at the deadline — retention keeps
// only the bounded post-detach tail, Remove cancels a pending expiry, and
// Drain/destructor expire ghosts immediately.

#include "smithy/server/session_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/eventstream/async_event_stream.h"
#include "smithy/eventstream/event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace opal::server {
namespace {

using eventstream::EventStream;
using eventstream::EventStreamHandle;
using eventstream::Message;
using eventstream::NoEvents;

// The hub's outbound event: what a game server pushes to each player.
struct Note {
  std::string text;
};

Outcome<Message> EncodeNote(const Note& note) {
  return Message{.headers = {{":event-type", "note"}}, .payload = Blob::FromString(note.text)};
}

using ServerStream = EventStream<Note, NoEvents>;
using Registry = SessionRegistry<Note>;

// The pair's per-direction bound, from the class: fill counts and margins
// derive from it so retuning the pair cannot silently invalidate them.
constexpr std::size_t kWireDepth = http::InMemoryWebSocketPair::kQueueDepth;

// One hub-side session: the server's stream (whose Share() feeds the
// registry) plus the client's raw end for observing what was delivered.
struct Session {
  std::shared_ptr<http::WebSocket> client;
  std::unique_ptr<ServerStream> stream;
  std::optional<EventStreamHandle<Note>> handle;
};

Session MakeSession() {
  auto [client_end, server_end] = http::InMemoryWebSocketPair::Create();
  Session session;
  session.client = client_end;
  session.stream = std::make_unique<ServerStream>(server_end, EncodeNote, nullptr);
  session.handle = session.stream->Share();
  return session;
}

// Drains one delivered event's payload at the client, or nullopt on the
// session's clean end.
std::optional<std::string> NextAt(Session& session) {
  auto message = session.client->Receive();
  if (!message.ok() || !message->has_value()) return std::nullopt;
  return (**message).payload.ToString();
}

// Forwards the blocking calls and hides the pair's async support — the
// per-entry writer-thread fallback the mixed-fleet contract promises.
class BlockingOnly final : public http::WebSocket {
 public:
  explicit BlockingOnly(std::shared_ptr<http::WebSocket> inner) : inner_(std::move(inner)) {}
  Outcome<std::optional<Message>> Receive() override { return inner_->Receive(); }
  Outcome<std::optional<Message>> Receive(std::chrono::milliseconds timeout) override {
    return inner_->Receive(timeout);
  }
  Outcome<Unit> Send(const Message& message) override { return inner_->Send(message); }
  void Close() override { inner_->Close(); }

 private:
  std::shared_ptr<http::WebSocket> inner_;
};

// MakeSession, but the server end advertises no async support, so an
// async-delivery registry gives this session a writer thread.
Session MakeBlockingOnlySession() {
  auto [client_end, server_end] = http::InMemoryWebSocketPair::Create();
  Session session;
  session.client = client_end;
  session.stream = std::make_unique<ServerStream>(std::make_shared<BlockingOnly>(server_end),
                                                  EncodeNote, nullptr);
  session.handle = session.stream->Share();
  return session;
}

// Polls SendTo until the registry refuses — the moment the writer notices a
// dead session is asynchronous; the deadline only bounds a failing test.
bool RefusedEventually(Registry& registry, const std::string& id) {
  for (int i = 0; i < 500; ++i) {
    if (!registry.SendTo(id, Note{"probe"})) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

TEST(SessionRegistryTest, SendToQueuesAndTheWriterDeliversInOrder) {
  Registry registry;
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(registry.SendTo("ada", Note{"note-" + std::to_string(i)}));
  }
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(NextAt(session), "note-" + std::to_string(i));
  }
  EXPECT_EQ(registry.size(), 1U);
  EXPECT_TRUE(registry.Remove("ada"));
}

TEST(SessionRegistryTest, SendToAnUnknownIdReportsFalse) {
  Registry registry;
  EXPECT_FALSE(registry.SendTo("nobody", Note{"lost"}));
  EXPECT_EQ(registry.size(), 0U);
}

TEST(SessionRegistryTest, ADuplicateAddIsRefusedAndHarmless) {
  Registry registry;
  Session first = MakeSession();
  Session second = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *first.handle));
  EXPECT_FALSE(registry.Add("ada", *second.handle));
  EXPECT_EQ(registry.size(), 1U);

  // The id kept its original session, and the refused handle was not
  // touched — its session still works directly.
  EXPECT_TRUE(registry.SendTo("ada", Note{"kept"}));
  EXPECT_EQ(NextAt(first), "kept");
  EXPECT_TRUE(second.handle->Send(Note{"untouched"}).ok());
  EXPECT_EQ(NextAt(second), "untouched");
}

TEST(SessionRegistryTest, BroadcastConstructsPerRecipient) {
  // The game-hub primitive: per-viewer redaction, not identical bytes.
  Registry registry;
  Session ada = MakeSession();
  Session grace = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *ada.handle));
  ASSERT_TRUE(registry.Add("grace", *grace.handle));

  const std::size_t queued = registry.Broadcast(
      {"ada", "grace", "ghost"}, [](const std::string& id) { return Note{"state-for-" + id}; });
  EXPECT_EQ(queued, 2U);  // the unknown id was skipped, not constructed for
  EXPECT_EQ(NextAt(ada), "state-for-ada");
  EXPECT_EQ(NextAt(grace), "state-for-grace");
}

TEST(SessionRegistryTest, BroadcastToEveryoneAndIdenticalBytesOverloads) {
  Registry registry;
  Session ada = MakeSession();
  Session grace = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *ada.handle));
  ASSERT_TRUE(registry.Add("grace", *grace.handle));

  EXPECT_EQ(registry.Broadcast(Note{"same-for-all"}), 2U);
  EXPECT_EQ(NextAt(ada), "same-for-all");
  EXPECT_EQ(NextAt(grace), "same-for-all");

  EXPECT_EQ(registry.Broadcast({"grace"}, Note{"targeted"}), 1U);
  EXPECT_EQ(NextAt(grace), "targeted");

  const auto ids = registry.Ids();
  EXPECT_EQ(ids, (std::vector<std::string>{"ada", "grace"}));
}

TEST(SessionRegistryTest, ASlowConsumerNeverBlocksTheBroadcasterAndIsClosed) {
  // ada reads; grace never does. Her wire bound and registry queue both
  // fill, after which enqueueing to grace drops and the default policy
  // closes her session — while ada keeps receiving and no Broadcast call
  // ever blocks.
  Registry::Options options;
  options.queue_capacity = 2;
  Registry registry(std::move(options));
  Session ada = MakeSession();
  Session grace = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *ada.handle));
  ASSERT_TRUE(registry.Add("grace", *grace.handle));

  constexpr int kBursts = 4 * kWireDepth;  // > wire bound + queue, with margin
  for (int i = 0; i < kBursts; ++i) {
    registry.Broadcast({"ada", "grace"}, Note{"burst-" + std::to_string(i)});
    EXPECT_EQ(NextAt(ada), "burst-" + std::to_string(i));  // never stalled
  }

  // grace's session was closed by the policy: her client drains what made
  // it onto the wire, then sees the close — not a stall.
  int delivered = 0;
  while (NextAt(grace).has_value()) ++delivered;
  EXPECT_LT(delivered, kBursts);

  // Her writer observed the failure; later sends to her report false. (The
  // id stays registered until its handler removes it — bookkeeping is the
  // application's.)
  EXPECT_TRUE(RefusedEventually(registry, "grace"));
  EXPECT_TRUE(registry.SendTo("ada", Note{"still-here"}));
  EXPECT_EQ(NextAt(ada), "still-here");
}

TEST(SessionRegistryTest, TheSlowConsumerCallbackReplacesTheCloseDefault) {
  std::atomic<int> slow_reports{0};
  Registry::Options options;
  options.queue_capacity = 1;
  options.on_slow_consumer = [&slow_reports](const std::string& id) {
    EXPECT_EQ(id, "grace");
    ++slow_reports;
  };
  Registry registry(std::move(options));
  Session grace = MakeSession();  // never reads
  ASSERT_TRUE(registry.Add("grace", *grace.handle));

  // Fill the wire + the queue (1); everything past that drops into the
  // callback instead of closing.
  int dropped = 0;
  for (std::size_t i = 0; i < 2 * kWireDepth; ++i) {
    if (!registry.SendTo("grace", Note{"n"})) ++dropped;
  }
  EXPECT_GT(dropped, 0);
  EXPECT_EQ(slow_reports.load(), dropped);

  // The session is still open — the application kept the policy.
  EXPECT_EQ(NextAt(grace), "n");
}

TEST(SessionRegistryTest, RemoveDiscardsUndeliveredButNeverCloses) {
  Registry::Options options;
  options.queue_capacity = 2 * kWireDepth;
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  // Stall the wire so events pile up: the writer can pop at most the wire
  // bound plus one blocked mid-Send before the client drains, and Remove
  // discards the rest outright.
  constexpr int kQueuedUp = kWireDepth + 4;
  for (int i = 0; i < kQueuedUp; ++i) {
    ASSERT_TRUE(registry.SendTo("ada", Note{"queued-" + std::to_string(i)}));
  }
  ASSERT_TRUE(registry.Remove("ada"));
  EXPECT_FALSE(registry.Remove("ada"));  // idempotence reports false
  EXPECT_FALSE(registry.SendTo("ada", Note{"after"}));

  // Remove is bookkeeping, not a close: the stream still works. The direct
  // send blocks behind the stalled wire, so it runs on the side while the
  // main thread drains the client.
  std::thread direct([&session] { EXPECT_TRUE(session.stream->Send(Note{"direct"}).ok()); });
  std::vector<std::string> received;
  for (;;) {
    const std::optional<std::string> note = NextAt(session);
    ASSERT_TRUE(note.has_value());  // a close before "direct" would be a bug
    if (*note == "direct") break;
    received.push_back(*note);
  }
  direct.join();

  // What arrived is a FIFO prefix; the tail Remove discarded never does.
  EXPECT_LT(received.size(), static_cast<std::size_t>(kQueuedUp));
  for (std::size_t i = 0; i < received.size(); ++i) {
    EXPECT_EQ(received[i], "queued-" + std::to_string(i));
  }
}

TEST(SessionRegistryTest, CloseAllEndsEverySessionAndDrainWaitsForRemoves) {
  // The drain recipe, once (issue #112 proposal 3): handlers block in
  // Receive; CloseAll wakes each, which removes itself and exits; Drain
  // reports the registry empty before the transport's abort-flavored Stop.
  Registry registry;
  constexpr int kSessions = 4;
  std::vector<std::thread> handlers;
  std::vector<Session> sessions(kSessions);
  for (int i = 0; i < kSessions; ++i) {
    sessions[i] = MakeSession();
    const std::string id = "player-" + std::to_string(i);
    ASSERT_TRUE(registry.Add(id, *sessions[i].handle));
    handlers.emplace_back([&registry, &session = sessions[i], id] {
      // The handler shape from the server guide: block serving until the
      // stream ends, then deregister on the way out.
      auto end = session.client->Receive();
      EXPECT_TRUE(end.ok());
      EXPECT_FALSE(end->has_value());
      registry.Remove(id);
    });
  }

  EXPECT_EQ(registry.size(), static_cast<std::size_t>(kSessions));
  EXPECT_TRUE(registry.Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(registry.size(), 0U);
  for (std::thread& handler : handlers) handler.join();
}

TEST(SessionRegistryTest, DrainReportsFalseWhenAHandlerForgetsRemove) {
  Registry registry;
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("forgetful", *session.handle));
  EXPECT_FALSE(registry.Drain(std::chrono::milliseconds(50)));
  EXPECT_EQ(registry.size(), 1U);  // still there for Stop() to abort
}

TEST(SessionRegistryTest, AStaleHandleStaysRegisteredWithoutDanger) {
  // The handler returned (stream destroyed) but forgot Remove — the exact
  // dangling-reference hazard of the borrowed-registry pattern this
  // replaces. Here it is a soft bug: delivery fails, nothing dangles.
  Registry registry;
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ghost", *session.handle));
  session.stream.reset();  // the borrow ends; the shared view is revoked

  EXPECT_TRUE(RefusedEventually(registry, "ghost"));
  EXPECT_EQ(registry.size(), 1U);
  EXPECT_TRUE(registry.Remove("ghost"));
}

TEST(SessionRegistryTest, TheDestructorClosesSessionsAndJoinsBlockedWriters) {
  // A registry destroyed mid-delivery: one writer is blocked on a stalled
  // wire, another has a full queue. The destructor must close both sessions
  // (unblocking the writers) and join every thread — this test hanging or
  // crashing is the failure mode.
  Session stalled = MakeSession();
  Session healthy = MakeSession();
  {
    Registry::Options options;
    options.queue_capacity = 4;
    Registry registry(std::move(options));
    ASSERT_TRUE(registry.Add("stalled", *stalled.handle));
    ASSERT_TRUE(registry.Add("healthy", *healthy.handle));
    for (std::size_t i = 0; i < 2 * kWireDepth; ++i) registry.SendTo("stalled", Note{"pile-up"});
    registry.SendTo("healthy", Note{"one"});
    EXPECT_EQ(NextAt(healthy), "one");
  }
  // Both clients observe their sessions' ends, proving the destructor
  // closed rather than abandoned them.
  while (NextAt(stalled).has_value()) {
  }
  EXPECT_FALSE(NextAt(healthy).has_value());
}

TEST(SessionRegistryTest, ConcurrentBroadcastsAddsAndRemovesStaySafe) {
  // Churn smoke: broadcasters race joins and leaves, wires stall, the
  // close-on-full default fires, stale handles get re-Added. Nothing here
  // asserts ordering — the test's job is to crash, hang, or trip TSan (the
  // sanitizer matrix runs it) if the registry's locking is wrong.
  Registry::Options options;
  options.queue_capacity = 2;
  Registry registry(std::move(options));
  std::atomic<bool> stop{false};
  std::vector<Session> sessions(8);
  for (int i = 0; i < 8; ++i) sessions[i] = MakeSession();

  std::thread churner([&] {
    for (int round = 0; round < 50; ++round) {
      for (int i = 0; i < 8; ++i) registry.Add("p" + std::to_string(i), *sessions[i].handle);
      for (int i = 0; i < 8; ++i) registry.Remove("p" + std::to_string(i));
    }
    stop = true;
  });
  std::thread broadcaster([&] {
    while (!stop) registry.Broadcast([](const std::string& id) { return Note{"tick-" + id}; });
  });

  churner.join();
  broadcaster.join();
  SUCCEED();  // the registry destructor closes and joins whatever remains
}

// ---------------------------------------------------------------------------
// Async delivery (ADR-0019): the same contracts, zero registry threads.
// ---------------------------------------------------------------------------

Registry AsyncRegistry(std::size_t queue_capacity = 2 * kWireDepth) {
  Registry::Options options;
  options.queue_capacity = queue_capacity;
  options.async_delivery = true;
  return Registry(std::move(options));
}

TEST(SessionRegistryAsyncTest, DeliversFifoThroughCompletionChains) {
  Registry registry = AsyncRegistry();
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  // Past the wire bound on purpose: the chain crosses its parked-send leg
  // (park on the full wire, absorb on drain), and FIFO must hold across it.
  constexpr int kEvents = static_cast<int>(2 * kWireDepth);
  for (int i = 0; i < kEvents; ++i) {
    EXPECT_TRUE(registry.SendTo("ada", Note{"note-" + std::to_string(i)}));
  }
  for (int i = 0; i < kEvents; ++i) {
    EXPECT_EQ(NextAt(session), "note-" + std::to_string(i));
  }
  EXPECT_TRUE(registry.Remove("ada"));
}

TEST(SessionRegistryAsyncTest, AChainCollisionWithADirectSendFallsBackToAWriterNotAStall) {
  Registry registry = AsyncRegistry();
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  // An application-side send owns the socket's send slot: fill the wire so
  // the next direct SendAsync parks in flight.
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    bool inline_ok = false;
    session.handle->SendAsync(Note{"fill"}, [&](const Outcome<Unit>& sent) {
      inline_ok = sent.ok();  // the pair completes inline while the wire has room
    });
    ASSERT_TRUE(inline_ok);
  }
  std::atomic<bool> parked_completed{false};
  session.handle->SendAsync(Note{"parked"}, [&](const Outcome<Unit>&) { parked_completed = true; });
  ASSERT_FALSE(parked_completed.load());  // in flight: the send slot is busy

  // The chain's kick collides with the in-flight send and is refused with
  // Validation. The collision must not kill delivery — and it must not
  // merely pause it either: nothing may depend on a rescuing later
  // enqueue, because sparse traffic never sends one (the session falls
  // back to a writer thread, which serializes by waiting).
  EXPECT_TRUE(registry.SendTo("ada", Note{"note-0"}));

  // Drain the wire: the fills, then the parked direct send completes.
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    EXPECT_EQ(NextAt(session), "fill");
  }
  EXPECT_EQ(NextAt(session), "parked");
  EXPECT_TRUE(parked_completed.load());

  // note-0 arrives with NO further registry activity. Deadlined receive:
  // a regression to pause-until-next-enqueue fails here instead of
  // hanging. The promise lives on the heap, owned by the callback, so an
  // early exit cannot dangle it.
  auto note0 = std::make_shared<std::promise<std::optional<std::string>>>();
  auto note0_future = note0->get_future();
  session.client->ReceiveAsync([note0](Outcome<std::optional<Message>> message) {
    if (message.ok() && message->has_value()) {
      note0->set_value((**message).payload.ToString());
    } else {
      note0->set_value(std::nullopt);
    }
  });
  ASSERT_EQ(note0_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "the collision stalled delivery: note-0 never arrived without a second enqueue";
  EXPECT_EQ(note0_future.get(), "note-0");

  // And the queue keeps flowing afterwards.
  EXPECT_TRUE(registry.SendTo("ada", Note{"note-1"}));
  EXPECT_EQ(NextAt(session), "note-1");
  EXPECT_TRUE(registry.Remove("ada"));
}

TEST(SessionRegistryAsyncTest, TheSlowConsumerCallbackReplacesTheCloseDefaultInAsyncMode) {
  std::atomic<int> slow_reports{0};
  Registry::Options options;
  options.queue_capacity = 1;
  options.async_delivery = true;
  options.on_slow_consumer = [&slow_reports](const std::string& id) {
    EXPECT_EQ(id, "grace");
    ++slow_reports;
  };
  Registry registry(std::move(options));
  Session grace = MakeSession();  // never reads
  ASSERT_TRUE(registry.Add("grace", *grace.handle));

  int dropped = 0;
  for (std::size_t i = 0; i < 2 * kWireDepth; ++i) {
    if (!registry.SendTo("grace", Note{"n"})) ++dropped;
  }
  EXPECT_GT(dropped, 0);
  EXPECT_EQ(slow_reports.load(), dropped);

  // The session is still open — the application kept the policy.
  EXPECT_EQ(NextAt(grace), "n");
}

TEST(SessionRegistryAsyncTest, CloseAllEndsEverySessionAndDrainWaitsForRemoves) {
  // The sync Drain contract, chain-delivered: CloseAll wakes each handler
  // through its closed session; each removes itself; Drain reports empty.
  Registry registry = AsyncRegistry();
  constexpr int kSessions = 4;
  std::vector<std::thread> handlers;
  std::vector<Session> sessions(kSessions);
  for (int i = 0; i < kSessions; ++i) {
    sessions[i] = MakeSession();
    const std::string id = "player-" + std::to_string(i);
    ASSERT_TRUE(registry.Add(id, *sessions[i].handle));
    handlers.emplace_back([&registry, &session = sessions[i], id] {
      auto end = session.client->Receive();
      EXPECT_TRUE(end.ok());
      EXPECT_FALSE(end->has_value());
      registry.Remove(id);
    });
  }

  EXPECT_EQ(registry.size(), static_cast<std::size_t>(kSessions));
  EXPECT_TRUE(registry.Drain(std::chrono::milliseconds(5000)));
  EXPECT_EQ(registry.size(), 0U);
  for (std::thread& handler : handlers) handler.join();
}

TEST(SessionRegistryAsyncTest, ASlowConsumerIsStillClosedNotWaitedFor) {
  Registry registry = AsyncRegistry(/*queue_capacity=*/2);
  Session ada = MakeSession();
  Session grace = MakeSession();  // never reads
  ASSERT_TRUE(registry.Add("ada", *ada.handle));
  ASSERT_TRUE(registry.Add("grace", *grace.handle));

  constexpr int kBursts = 4 * kWireDepth;
  for (int i = 0; i < kBursts; ++i) {
    registry.Broadcast({"ada", "grace"}, Note{"burst-" + std::to_string(i)});
    EXPECT_EQ(NextAt(ada), "burst-" + std::to_string(i));  // never stalled
  }
  int delivered = 0;
  while (NextAt(grace).has_value()) ++delivered;
  EXPECT_LT(delivered, kBursts);
  EXPECT_TRUE(RefusedEventually(registry, "grace"));
}

TEST(SessionRegistryAsyncTest, ABlockingOnlySocketFallsBackToAWriterThread) {
  Registry registry = AsyncRegistry();
  Session session = MakeBlockingOnlySession();
  ASSERT_FALSE(session.handle->SupportsAsync());
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(registry.SendTo("ada", Note{"fallback-" + std::to_string(i)}));
  }
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(NextAt(session), "fallback-" + std::to_string(i));
  }
  EXPECT_TRUE(registry.Remove("ada"));
}

TEST(SessionRegistryAsyncTest, TeardownWithAParkedChainNeverHangs) {
  // The wire fills, the chain parks inside the socket's SendAsync, and the
  // registry is destroyed: its close fails the parked delivery, the chain
  // stops, and — with no writer threads — there is nothing to join. This
  // test hanging or crashing is the failure mode.
  Session stalled = MakeSession();
  {
    Registry registry = AsyncRegistry();
    ASSERT_TRUE(registry.Add("stalled", *stalled.handle));
    for (std::size_t i = 0; i < 2 * kWireDepth; ++i) {
      registry.SendTo("stalled", Note{"pile-up"});
    }
  }
  while (NextAt(stalled).has_value()) {
  }
  SUCCEED();
}

// The whole composition a hub actually runs (ADR-0019 + ADR-0017): a
// Detached loop owning the session, its Share() handle in an
// async_delivery registry, and a peer that closes while a fan-out delivery
// is still parked on the wire. This is the shape #173 wedged, and the one
// no test had: the transport suites park waiters without a registry above
// them, and the registry suites park chains without a coroutine loop
// below. The bug needed both halves at once.
using AsyncServerStream = eventstream::AsyncEventStream<Note, Message>;

Outcome<Message> DecodeAnything(const Message& message) { return message; }

TEST(SessionRegistryAsyncTest, APeerCloseWithAParkedFanOutEndsTheLoopAndTheDelivery) {
  // The loop parks in Receive; the chain parks inside SendAsync on the full
  // wire, holding its revocation pin. The peer's close then has to complete
  // the delivery, resume the loop, and let ~AsyncEventStream drain that pin
  // — all on the closing thread, in that order. This test hanging is the
  // failure mode.
  auto [client_end, server_end] = http::InMemoryWebSocketPair::Create();
  Registry registry = AsyncRegistry();
  std::atomic<bool> loop_ended{false};
  std::promise<EventStreamHandle<Note>> minted;
  auto handle_ready = minted.get_future();

  [](std::shared_ptr<http::WebSocket> socket, std::promise<EventStreamHandle<Note>>* minted,
     std::atomic<bool>* ended) -> eventstream::Detached {
    AsyncServerStream stream(std::move(socket), EncodeNote, DecodeAnything);
    minted->set_value(stream.Share());
    (void)co_await stream.Receive();  // parked until the peer closes
    *ended = true;
  }(server_end, &minted, &loop_ended);

  ASSERT_EQ(handle_ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_TRUE(registry.Add("ada", handle_ready.get()));

  // Past the wire bound on purpose: the wire takes what it can and the
  // chain parks on the next delivery, which is where the pin lives.
  for (std::size_t i = 0; i < 2 * kWireDepth; ++i) {
    ASSERT_TRUE(registry.SendTo("ada", Note{"pile-up"}));
  }

  // The pair runs the entire teardown on whoever calls Close, so keep it
  // off the test thread — a wedge there cannot even be joined.
  std::promise<void> closed;
  auto closed_future = closed.get_future();
  std::thread closer([&] {
    client_end->Close();
    closed.set_value();
  });
  if (closed_future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
    ADD_FAILURE() << "the close wedged: a parked fan-out delivery was never completed";
    std::abort();  // the closer cannot be joined, and the loop still holds this frame
  }
  closer.join();

  for (int i = 0; i < 1000 && !loop_ended.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!loop_ended.load()) {
    ADD_FAILURE() << "the session loop never unwound";
    std::abort();
  }
  EXPECT_TRUE(registry.Remove("ada"));  // the entry outlived the session, harmlessly
}

TEST(SessionRegistryAsyncTest, ChurnUnderBroadcastStaysSafe) {
  Registry registry = AsyncRegistry(/*queue_capacity=*/2);
  std::atomic<bool> stop{false};
  std::vector<Session> sessions(8);
  for (int i = 0; i < 8; ++i) sessions[i] = MakeSession();

  std::thread churner([&] {
    for (int round = 0; round < 50; ++round) {
      for (int i = 0; i < 8; ++i) registry.Add("p" + std::to_string(i), *sessions[i].handle);
      for (int i = 0; i < 8; ++i) registry.Remove("p" + std::to_string(i));
    }
    stop = true;
  });
  std::thread broadcaster([&] {
    while (!stop) registry.Broadcast([](const std::string& id) { return Note{"tick-" + id}; });
  });
  churner.join();
  broadcaster.join();
  SUCCEED();
}

// ---------------------------------------------------------------------------
// Reconnect grace (ADR-0020): Detach/Resume, expiry, and their races.
// ---------------------------------------------------------------------------

// A grace-enabled registry whose on_expired records into the caller's
// atomics. One-second grace: the smallest the seconds-granularity option
// expresses, so expiry tests wait ~1s, bounded by the poll deadlines.
Registry GraceRegistry(std::atomic<int>* expired_count, std::string* expired_id = nullptr,
                       bool async_delivery = false, bool queue_while_detached = false,
                       std::size_t queue_capacity = 2 * kWireDepth) {
  Registry::Options options;
  options.queue_capacity = queue_capacity;
  options.async_delivery = async_delivery;
  options.queue_while_detached = queue_while_detached;
  options.grace_period = std::chrono::seconds{1};
  options.on_expired = [expired_count, expired_id](const std::string& id) {
    if (expired_id != nullptr) *expired_id = id;
    ++*expired_count;
  };
  return Registry(std::move(options));
}

bool ExpiredEventually(const std::atomic<int>& count, int at_least) {
  for (int i = 0; i < 500; ++i) {
    if (count.load() >= at_least) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

TEST(SessionRegistryGraceTest, DetachAndResumeRequireAGracePeriod) {
  Registry registry;  // grace disabled (the default): prior behavior verbatim
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  EXPECT_FALSE(registry.Detach("ada"));
  Session fresh = MakeSession();
  EXPECT_FALSE(registry.Resume("ada", *fresh.handle));
  EXPECT_TRUE(registry.SendTo("ada", Note{"still-live"}));
  EXPECT_EQ(NextAt(session), "still-live");
}

TEST(SessionRegistryGraceTest, DetachParksAndResumeSwapsInTheNewConnection) {
  std::atomic<int> expired{0};
  // Long grace: expiry can never race the assertions below.
  Registry::Options options;
  options.on_expired = [&expired](const std::string&) { ++expired; };
  options.grace_period = std::chrono::seconds{300};
  Registry slow(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(slow.Add("ada", *session.handle));
  ASSERT_TRUE(slow.SendTo("ada", Note{"before"}));
  EXPECT_EQ(NextAt(session), "before");

  ASSERT_TRUE(slow.Detach("ada"));
  EXPECT_FALSE(slow.Detach("ada"));  // already detached
  // The old connection was closed by the detach; the entry stays counted.
  EXPECT_EQ(NextAt(session), std::nullopt);
  EXPECT_EQ(slow.size(), 1U);
  EXPECT_EQ(slow.Ids(), (std::vector<std::string>{"ada"}));  // still registered
  // Default posture: events to a detached id are dropped, reported unqueued.
  EXPECT_FALSE(slow.SendTo("ada", Note{"lost"}));
  EXPECT_EQ(slow.Broadcast(Note{"lost-too"}), 0U);  // detached ids count as unqueued

  // The identity-keyed swap: the new connection takes over the entry.
  Session fresh = MakeSession();
  ASSERT_TRUE(slow.Resume("ada", *fresh.handle));
  EXPECT_TRUE(slow.SendTo("ada", Note{"after"}));
  EXPECT_EQ(NextAt(fresh), "after");
  EXPECT_EQ(slow.size(), 1U);
  EXPECT_TRUE(slow.Remove("ada"));
  EXPECT_EQ(expired.load(), 0);
}

TEST(SessionRegistryGraceTest, ResumeRefusesLiveAndUnknownIds) {
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  Session fresh = MakeSession();
  EXPECT_FALSE(registry.Resume("ada", *fresh.handle));    // live: the app decides
  EXPECT_FALSE(registry.Resume("ghost", *fresh.handle));  // unknown
  EXPECT_TRUE(registry.SendTo("ada", Note{"untouched"}));
  EXPECT_EQ(NextAt(session), "untouched");
}

TEST(SessionRegistryGraceTest, ExpiryRunsOnExpiredExactlyOnceAndRemoves) {
  std::atomic<int> expired{0};
  std::string expired_id;
  Registry registry = GraceRegistry(&expired, &expired_id);
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.Detach("ada"));

  ASSERT_TRUE(ExpiredEventually(expired, 1));
  EXPECT_EQ(expired.load(), 1);
  EXPECT_EQ(expired_id, "ada");
  EXPECT_EQ(registry.size(), 0U);
  Session fresh = MakeSession();
  EXPECT_FALSE(registry.Resume("ada", *fresh.handle));  // expiry claimed it
}

TEST(SessionRegistryGraceTest, ResumeAndExpiryAreMutuallyExclusive) {
  // The race the Go hub got wrong, run right at the deadline: whatever
  // the interleaving, exactly one of {successful resume, on_expired}
  // happens per detach.
  for (int i = 0; i < 6; ++i) {
    std::atomic<int> expired{0};
    Registry registry = GraceRegistry(&expired);
    Session session = MakeSession();
    ASSERT_TRUE(registry.Add("ada", *session.handle));
    ASSERT_TRUE(registry.Detach("ada"));

    // Land the resume attempt around the 1s deadline, sweeping the window.
    std::this_thread::sleep_for(std::chrono::milliseconds(900 + 40 * i));
    Session fresh = MakeSession();
    const bool resumed = registry.Resume("ada", *fresh.handle);
    if (resumed) {
      // Expiry must never also fire: wait past any pending deadline.
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      EXPECT_EQ(expired.load(), 0) << "iteration " << i;
      EXPECT_TRUE(registry.SendTo("ada", Note{"resumed"}));
      EXPECT_EQ(NextAt(fresh), "resumed");
    } else {
      ASSERT_TRUE(ExpiredEventually(expired, 1)) << "iteration " << i;
      EXPECT_EQ(expired.load(), 1) << "iteration " << i;
      EXPECT_EQ(registry.size(), 0U);
    }
  }
}

TEST(SessionRegistryGraceTest, ConcurrentResumesExactlyOneWins) {
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.Detach("ada"));

  // Two dials claim the same id at once — the second-device race.
  Session first = MakeSession();
  Session second = MakeSession();
  std::atomic<int> wins{0};
  std::thread a([&] {
    if (registry.Resume("ada", *first.handle)) ++wins;
  });
  std::thread b([&] {
    if (registry.Resume("ada", *second.handle)) ++wins;
  });
  a.join();
  b.join();
  EXPECT_EQ(wins.load(), 1);
  // The winner's wire delivers; exactly one of the two clients gets it.
  // (CloseAll only reaches the registered winner, so the loser's receive
  // is unblocked by closing the client ends once the delivery landed.)
  EXPECT_TRUE(registry.SendTo("ada", Note{"winner"}));
  auto at_first = std::async(std::launch::async, [&] { return NextAt(first); });
  auto at_second = std::async(std::launch::async, [&] { return NextAt(second); });
  std::optional<std::string> delivered;
  bool first_taken = false;
  for (int i = 0; i < 500 && !delivered.has_value(); ++i) {
    if (at_first.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready) {
      delivered = at_first.get();
      first_taken = true;
      break;
    }
    if (at_second.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready) {
      delivered = at_second.get();
      break;
    }
  }
  EXPECT_EQ(delivered, "winner");
  first.client->Close();
  second.client->Close();
  EXPECT_EQ((first_taken ? at_second : at_first).get(), std::nullopt);  // the loser saw nothing
}

TEST(SessionRegistryGraceTest, RemoveOnDetachedIsImmediateAndNeverExpires) {
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.Detach("ada"));
  EXPECT_TRUE(registry.Remove("ada"));  // leave/kick during grace
  EXPECT_EQ(registry.size(), 0U);
  // The claim was Remove's; the expiry thread finds nothing to take.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_EQ(expired.load(), 0);
}

TEST(SessionRegistryGraceTest, DrainExpiresDetachedImmediately) {
  std::atomic<int> expired{0};
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};  // Drain must not wait this out
  options.on_expired = [&expired](const std::string&) { ++expired; };
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.Detach("ada"));

  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(registry.Drain(std::chrono::milliseconds(5000)));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_EQ(expired.load(), 1);
  EXPECT_EQ(registry.size(), 0U);
}

TEST(SessionRegistryGraceTest, OptInRetentionDeliversTheBoundedTailOnResume) {
  // Long grace so expiry cannot race the retention checks.
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.queue_while_detached = true;
  options.queue_capacity = 3;
  Registry retaining(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(retaining.Add("ada", *session.handle));
  ASSERT_TRUE(retaining.Detach("ada"));

  // Three fit the bounded queue; the rest drop outright (no policy — no
  // live session to act on).
  EXPECT_TRUE(retaining.SendTo("ada", Note{"tail-0"}));
  EXPECT_TRUE(retaining.SendTo("ada", Note{"tail-1"}));
  EXPECT_TRUE(retaining.SendTo("ada", Note{"tail-2"}));
  EXPECT_FALSE(retaining.SendTo("ada", Note{"overflow"}));

  Session fresh = MakeSession();
  ASSERT_TRUE(retaining.Resume("ada", *fresh.handle));
  EXPECT_EQ(NextAt(fresh), "tail-0");
  EXPECT_EQ(NextAt(fresh), "tail-1");
  EXPECT_EQ(NextAt(fresh), "tail-2");
}

TEST(SessionRegistryGraceTest, AsyncChainDetachesAndResumesWithTheRetainedTail) {
  std::atomic<int> expired{0};
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.async_delivery = true;
  options.queue_while_detached = true;
  options.queue_capacity = 2 * kWireDepth;
  options.on_expired = [&expired](const std::string&) { ++expired; };
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.SendTo("ada", Note{"chained"}));
  EXPECT_EQ(NextAt(session), "chained");

  ASSERT_TRUE(registry.Detach("ada"));
  EXPECT_TRUE(registry.SendTo("ada", Note{"tail"}));  // retained, not kicked

  Session fresh = MakeSession();
  ASSERT_TRUE(registry.Resume("ada", *fresh.handle));  // re-kicks the chain
  EXPECT_EQ(NextAt(fresh), "tail");
  EXPECT_TRUE(registry.SendTo("ada", Note{"flowing"}));
  EXPECT_EQ(NextAt(fresh), "flowing");
  EXPECT_EQ(expired.load(), 0);
}

TEST(SessionRegistryGraceTest, DestructorExpiresDetachedEntries) {
  std::atomic<int> expired{0};
  {
    Registry::Options options;
    options.grace_period = std::chrono::seconds{300};
    options.on_expired = [&expired](const std::string&) { ++expired; };
    Registry registry(std::move(options));
    Session session = MakeSession();
    ASSERT_TRUE(registry.Add("ada", *session.handle));
    ASSERT_TRUE(registry.Detach("ada"));
  }  // ~SessionRegistry: no five-minute wait, cleanup still promised
  EXPECT_EQ(expired.load(), 1);
}

TEST(SessionRegistryGraceTest, ResumeOrAddResumesADetachedEntry) {
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.Detach("ada"));

  Session fresh = MakeSession();
  const auto admission =
      registry.ResumeOrAdd("ada", [&fresh] { return *fresh.handle; }, std::chrono::milliseconds(0));
  EXPECT_EQ(admission, Registry::Admission::kResumed);
  EXPECT_TRUE(registry.SendTo("ada", Note{"back"}));
  EXPECT_EQ(NextAt(fresh), "back");
}

TEST(SessionRegistryGraceTest, ResumeOrAddAddsAFreshId) {
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  const auto admission = registry.ResumeOrAdd(
      "ada", [&session] { return *session.handle; }, std::chrono::milliseconds(0));
  EXPECT_EQ(admission, Registry::Admission::kAdded);
  EXPECT_TRUE(registry.SendTo("ada", Note{"joined"}));
  EXPECT_EQ(NextAt(session), "joined");
}

TEST(SessionRegistryGraceTest, ResumeOrAddRefusesALiveIdOnlyAtTheDeadline) {
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  Session imposter = MakeSession();
  const auto start = std::chrono::steady_clock::now();
  const auto admission = registry.ResumeOrAdd(
      "ada", [&imposter] { return *imposter.handle; }, std::chrono::milliseconds(200));
  EXPECT_EQ(admission, Registry::Admission::kRefused);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
  // The live session was never touched — refusal, not a kick.
  EXPECT_TRUE(registry.SendTo("ada", Note{"untouched"}));
  EXPECT_EQ(NextAt(session), "untouched");
}

TEST(SessionRegistryGraceTest, AZeroDeadlineAdmissionIsSingleShot) {
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  Session imposter = MakeSession();
  const auto start = std::chrono::steady_clock::now();
  const auto admission = registry.ResumeOrAdd(
      "ada", [&imposter] { return *imposter.handle; }, std::chrono::milliseconds(0));
  EXPECT_EQ(admission, Registry::Admission::kRefused);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST(SessionRegistryGraceTest, AnEffectivelyInfiniteDeadlineNeitherOverflowsNorRefusesEarly) {
  // milliseconds::max() is the natural "block until admitted" spelling;
  // naive give_up arithmetic overflows the steady clock's rep (UB, and a
  // wrapped deadline silently degrades to single-shot). The sanitizer
  // build is the real referee here.
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  const auto admission = registry.ResumeOrAdd(
      "ada", [&session] { return *session.handle; }, std::chrono::milliseconds::max());
  EXPECT_EQ(admission, Registry::Admission::kAdded);
  EXPECT_TRUE(registry.SendTo("ada", Note{"in"}));
  EXPECT_EQ(NextAt(session), "in");
}

TEST(SessionRegistryGraceTest, MintRunsOncePerAttempt) {
  // The documented contract: one fresh handle per attempt serves both the
  // Resume try and the Add try (handles are cheap-copy views); a mint
  // with side effects sees exactly one call per attempt.
  std::atomic<int> expired{0};
  Registry registry = GraceRegistry(&expired);
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));  // the id is live

  Session imposter = MakeSession();
  int mints = 0;
  const auto admission = registry.ResumeOrAdd(
      "ada",
      [&] {
        ++mints;
        return *imposter.handle;
      },
      std::chrono::milliseconds(0));
  EXPECT_EQ(admission, Registry::Admission::kRefused);
  EXPECT_EQ(mints, 1);  // single-shot AND single-mint: one attempt, one handle
}

TEST(SessionRegistryGraceTest, ConcurrentResumeOrAddsAdmitExactlyOne) {
  // The invariant the primitive inherits from Resume's under-both-locks
  // swap and Add's emplace: two racers on one id admit exactly once.
  for (int round = 0; round < 10; ++round) {
    Registry::Options options;
    options.grace_period = std::chrono::seconds{300};
    Registry registry(std::move(options));
    Session session = MakeSession();
    ASSERT_TRUE(registry.Add("ada", *session.handle));
    ASSERT_TRUE(registry.Detach("ada"));  // one resumable seat

    Session first = MakeSession();
    Session second = MakeSession();
    Registry::Admission a{};
    Registry::Admission b{};
    std::thread racer_a([&] {
      a = registry.ResumeOrAdd(
          "ada", [&first] { return *first.handle; }, std::chrono::milliseconds(200));
    });
    std::thread racer_b([&] {
      b = registry.ResumeOrAdd(
          "ada", [&second] { return *second.handle; }, std::chrono::milliseconds(200));
    });
    racer_a.join();
    racer_b.join();
    const bool a_won = a == Registry::Admission::kResumed;
    const bool b_won = b == Registry::Admission::kResumed;
    EXPECT_NE(a_won, b_won) << "round " << round;  // exactly one resumed
    EXPECT_EQ(a_won ? b : a, Registry::Admission::kRefused) << "round " << round;
  }
}

TEST(SessionRegistryGraceTest, ResumeOrAddDegradesToRetriedAddWithoutGrace) {
  // grace_period == 0: Resume can never succeed, so the primitive is a
  // retried Add — still the right admission shape (the retry window lets
  // the old handler's Remove land), with kResumed unreachable.
  Registry registry;  // grace disabled
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  std::thread late_remove([&registry] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(registry.Remove("ada"));
  });
  Session fresh = MakeSession();
  const auto admission =
      registry.ResumeOrAdd("ada", [&fresh] { return *fresh.handle; }, std::chrono::seconds(5));
  late_remove.join();
  EXPECT_EQ(admission, Registry::Admission::kAdded);
  EXPECT_TRUE(registry.SendTo("ada", Note{"fresh-join"}));
  EXPECT_EQ(NextAt(fresh), "fresh-join");
}

TEST(SessionRegistryGraceTest, ResumeOrAddConvergesWhenDetachLandsMidRetry) {
  // The race the primitive exists for: the reconnect arrives while the old
  // handler has not yet noticed its dead wire — the id looks live to both
  // paths — and the Detach lands mid-retry. ResumeOrAdd must converge on
  // kResumed, never a refusal and never a duplicate join.
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  std::thread late_detach([&registry] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(registry.Detach("ada"));  // EXPECT: fatal asserts cannot halt a sibling thread
  });
  Session fresh = MakeSession();
  const auto admission =
      registry.ResumeOrAdd("ada", [&fresh] { return *fresh.handle; }, std::chrono::seconds(5));
  late_detach.join();
  EXPECT_EQ(admission, Registry::Admission::kResumed);
  EXPECT_TRUE(registry.SendTo("ada", Note{"converged"}));
  EXPECT_EQ(NextAt(fresh), "converged");
}

TEST(SessionRegistryGraceTest, CloseKicksTheLiveSessionAndFreesTheId) {
  // The kick primitive (ADR-0022): the session observes the close, the
  // handler side runs its normal exit path, and the id frees.
  Registry registry;
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));

  EXPECT_TRUE(registry.Close("ada"));
  EXPECT_EQ(NextAt(session), std::nullopt);  // the kicked wire observes it
  EXPECT_TRUE(registry.Remove("ada"));       // the exit path the handler runs
  Session fresh = MakeSession();
  EXPECT_TRUE(registry.Add("ada", *fresh.handle));  // the id freed
  EXPECT_FALSE(registry.Close("ghost"));
}

TEST(SessionRegistryGraceTest, CloseOnADetachedEntryIsAHarmlessNoOp) {
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.Detach("ada"));

  EXPECT_TRUE(registry.Close("ada"));  // found; the handle was already closed
  // The entry stays parked and resumable — Close did not disturb grace.
  Session fresh = MakeSession();
  ASSERT_TRUE(registry.Resume("ada", *fresh.handle));
  EXPECT_TRUE(registry.SendTo("ada", Note{"still-here"}));
  EXPECT_EQ(NextAt(fresh), "still-here");
}

TEST(SessionRegistryGraceTest, AThrowingOnExpiredIsSwallowedAndExpiryCompletes) {
  // on_expired runs on the registry's expiry thread (or a Drain caller),
  // where an escaping exception has no caller to land in — it must be
  // swallowed, and the batch must keep going. Pre-guard this test dies in
  // std::terminate.
  std::atomic<int> expired{0};
  Registry::Options options;
  options.grace_period = std::chrono::seconds{1};
  options.on_expired = [&expired](const std::string&) {
    ++expired;
    throw std::runtime_error("policy bug");
  };
  Registry registry(std::move(options));
  Session ada = MakeSession();
  Session bob = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *ada.handle));
  ASSERT_TRUE(registry.Add("bob", *bob.handle));
  ASSERT_TRUE(registry.Detach("ada"));
  ASSERT_TRUE(registry.Detach("bob"));
  ASSERT_TRUE(ExpiredEventually(expired, 2));  // the first throw killed nothing
  EXPECT_EQ(registry.size(), 0U);
}

TEST(SessionRegistryGraceTest, DetachResumeChurnUnderBroadcastAndCloseStaysSafe) {
  // The handle-mutability race Resume introduced: Enqueue's close-on-full
  // default and CloseAll both close entry->handle from foreign threads
  // while Resume swaps it. Meaningful under TSan (the sanitizer run), a
  // crash/hang smoke test elsewhere.
  std::vector<Session> keep_alive;  // outlives the registry below
  keep_alive.reserve(240);
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.queue_capacity = 1;  // every broadcast burst hits close-on-full
  Registry registry(std::move(options));

  std::atomic<bool> stop{false};
  std::atomic<int> choreography_failures{0};
  std::thread churner([&] {
    for (int round = 0; round < 30; ++round) {
      for (int i = 0; i < 4; ++i) {
        const std::string id = "p" + std::to_string(i);
        Session joined = MakeSession();
        if (!registry.Add(id, *joined.handle)) ++choreography_failures;
        keep_alive.push_back(std::move(joined));
        if (!registry.Detach(id)) ++choreography_failures;
        Session back = MakeSession();
        if (!registry.Resume(id, *back.handle)) ++choreography_failures;
        keep_alive.push_back(std::move(back));
        if (!registry.Remove(id)) ++choreography_failures;
      }
    }
    stop = true;
  });
  std::thread broadcaster([&] {
    while (!stop) registry.Broadcast(Note{"tick"});
  });
  std::thread closer([&] {
    while (!stop) {
      registry.CloseAll();
      for (int i = 0; i < 4; ++i) registry.Close("p" + std::to_string(i));  // the ADR-0022 path
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  churner.join();
  broadcaster.join();
  closer.join();
  EXPECT_EQ(choreography_failures.load(), 0);
}

TEST(SessionRegistryGraceTest, ResumeWaitsOutAWedgedWriterAndDeliversTheRetainedTail) {
  // The non-trivial quiescence wait: Detach lands while the writer is
  // blocked inside Send on a full wire. Resume must wait out the failing
  // send, join the writer, and hand the retained tail to the new
  // connection — minus the mid-send event, which died with the old wire
  // (by design: pinned here).
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.queue_while_detached = true;
  options.queue_capacity = 2 * kWireDepth;
  Registry registry(std::move(options));
  Session session = MakeSession();  // never reads: the wire will fill
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  for (std::size_t i = 0; i < kWireDepth + 4; ++i) {
    ASSERT_TRUE(registry.SendTo("ada", Note{"n-" + std::to_string(i)}));
  }
  // The in-memory pair delivers in microseconds; by now the writer has
  // filled the wire and sits blocked inside Send on event kWireDepth.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ASSERT_TRUE(registry.Detach("ada"));  // fails the blocked send out
  Session fresh = MakeSession();
  ASSERT_TRUE(registry.Resume("ada", *fresh.handle));
  for (std::size_t i = kWireDepth + 1; i < kWireDepth + 4; ++i) {
    EXPECT_EQ(NextAt(fresh), "n-" + std::to_string(i));
  }
  EXPECT_TRUE(registry.SendTo("ada", Note{"flowing"}));
  EXPECT_EQ(NextAt(fresh), "flowing");
}

TEST(SessionRegistryGraceTest, RepeatedDetachResumeCyclesJoinEachWriter) {
  // Resume resets stopping/done for the writer it spawns; a regression
  // there would let a later cycle's wait pass while the previous writer
  // still delivers, swapping the handle under an active Send. Every cycle
  // after the first exercises the reset state; the destructor closes out
  // whatever a leak would leave behind.
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.queue_while_detached = true;
  options.queue_capacity = 2 * kWireDepth;
  Registry registry(std::move(options));
  Session current = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *current.handle));
  for (int cycle = 0; cycle < 3; ++cycle) {
    const std::string tag = std::to_string(cycle);
    for (std::size_t i = 0; i < kWireDepth + 2; ++i) {
      ASSERT_TRUE(registry.SendTo("ada", Note{"c" + tag + "-" + std::to_string(i)}));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // writer wedges mid-Send
    ASSERT_TRUE(registry.Detach("ada"));
    Session fresh = MakeSession();
    ASSERT_TRUE(registry.Resume("ada", *fresh.handle));
    // The tail behind the mid-send casualty survives into the new session.
    EXPECT_EQ(NextAt(fresh), "c" + tag + "-" + std::to_string(kWireDepth + 1));
    current = std::move(fresh);
  }
  EXPECT_TRUE(registry.SendTo("ada", Note{"alive"}));
  EXPECT_EQ(NextAt(current), "alive");
}

TEST(SessionRegistryGraceTest, AsyncDetachWithAParkedChainRetainsAndResumesTheTail) {
  // The chain-mode counterpart of the wedged writer: Detach lands while
  // the chain is parked inside SendAsync on a full wire. The asymmetry
  // with the writer is deliberate and pinned here: the in-flight event
  // stays at the queue front (popped only on success), so nothing is lost
  // — the whole tail arrives after Resume.
  std::atomic<int> expired{0};
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.async_delivery = true;
  options.queue_while_detached = true;
  options.queue_capacity = 2 * kWireDepth;
  options.on_expired = [&expired](const std::string&) { ++expired; };
  Registry registry(std::move(options));
  Session session = MakeSession();  // never reads: the chain will park
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  // The pair's ready path completes inline, so these sends deliver as they
  // enqueue until the wire fills; the chain then parks in flight on event
  // kWireDepth with the rest queued behind it. Deterministic — no settle.
  for (std::size_t i = 0; i < kWireDepth + 3; ++i) {
    ASSERT_TRUE(registry.SendTo("ada", Note{"n-" + std::to_string(i)}));
  }

  ASSERT_TRUE(registry.Detach("ada"));  // fails the parked completion out
  Session fresh = MakeSession();
  ASSERT_TRUE(registry.Resume("ada", *fresh.handle));  // waits out `delivering`
  for (std::size_t i = kWireDepth; i < kWireDepth + 3; ++i) {
    EXPECT_EQ(NextAt(fresh), "n-" + std::to_string(i));
  }
  EXPECT_TRUE(registry.SendTo("ada", Note{"flowing"}));
  EXPECT_EQ(NextAt(fresh), "flowing");
  EXPECT_EQ(expired.load(), 0);
}

TEST(SessionRegistryGraceTest, AConvertedWritersWedgeIsJoinedByResume) {
  // Collision fallback meets grace: a direct send holds the socket's slot,
  // the chain's kick is refused and converts the session to a writer,
  // that writer wedges behind the direct send — and Detach/Resume must
  // wait out and join the CONVERTED writer, then re-arm per the new
  // handle's capability (back to a chain here).
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.async_delivery = true;
  options.queue_while_detached = true;
  options.queue_capacity = 2 * kWireDepth;
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    session.handle->SendAsync(Note{"fill"}, [](const Outcome<Unit>&) {});
  }
  std::atomic<bool> parked_failed{false};
  session.handle->SendAsync(Note{"parked"},
                            [&](const Outcome<Unit>& sent) { parked_failed = !sent.ok(); });

  ASSERT_TRUE(registry.SendTo("ada", Note{"tail-0"}));          // refused → converts
  ASSERT_TRUE(registry.SendTo("ada", Note{"tail-1"}));          // queued behind the wedge
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the writer wedges

  ASSERT_TRUE(registry.Detach("ada"));  // fails the direct send and the writer out
  EXPECT_TRUE(parked_failed.load());
  Session fresh = MakeSession();
  ASSERT_TRUE(registry.Resume("ada", *fresh.handle));
  // tail-0 was the converted writer's mid-send casualty; tail-1 survives.
  EXPECT_EQ(NextAt(fresh), "tail-1");
  EXPECT_TRUE(registry.SendTo("ada", Note{"flowing"}));
  EXPECT_EQ(NextAt(fresh), "flowing");
}

TEST(SessionRegistryGraceTest, ResumeRearmsDeliveryPerTheNewHandlesCapability) {
  // Resume recomputes the delivery mode from the NEW handle: a writer-mode
  // entry resumed with an async-capable connection goes chain (no writer
  // spawned, the retained tail kicked), and the reverse spawns a writer
  // for an entry that never had one.
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.async_delivery = true;
  options.queue_while_detached = true;
  Registry registry(std::move(options));
  Session blocking = MakeBlockingOnlySession();  // writer mode under async delivery
  ASSERT_TRUE(registry.Add("ada", *blocking.handle));
  ASSERT_TRUE(registry.SendTo("ada", Note{"writer-mode"}));
  EXPECT_EQ(NextAt(blocking), "writer-mode");

  ASSERT_TRUE(registry.Detach("ada"));
  ASSERT_TRUE(registry.SendTo("ada", Note{"tail-0"}));
  Session chained = MakeSession();  // async-capable
  ASSERT_TRUE(registry.Resume("ada", *chained.handle));
  EXPECT_EQ(NextAt(chained), "tail-0");
  ASSERT_TRUE(registry.SendTo("ada", Note{"chain-mode"}));
  EXPECT_EQ(NextAt(chained), "chain-mode");

  ASSERT_TRUE(registry.Detach("ada"));
  ASSERT_TRUE(registry.SendTo("ada", Note{"tail-1"}));
  Session writer_again = MakeBlockingOnlySession();
  ASSERT_TRUE(registry.Resume("ada", *writer_again.handle));
  EXPECT_EQ(NextAt(writer_again), "tail-1");
  ASSERT_TRUE(registry.SendTo("ada", Note{"writer-again"}));
  EXPECT_EQ(NextAt(writer_again), "writer-again");
}

TEST(SessionRegistryGraceTest, StaggeredDetachesExpireInDeadlineOrder) {
  // Two detached entries at once: the expiry thread re-derives the nearest
  // deadline each wake, so B's later Detach must neither delay A past its
  // deadline nor ride along with it.
  std::atomic<int> expired{0};
  std::mutex order_mutex;
  std::vector<std::string> order;
  Registry::Options options;
  options.grace_period = std::chrono::seconds{1};
  options.on_expired = [&](const std::string& id) {
    const std::lock_guard<std::mutex> lock(order_mutex);
    order.push_back(id);
    ++expired;
  };
  Registry registry(std::move(options));
  Session ada = MakeSession();
  Session bob = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *ada.handle));
  ASSERT_TRUE(registry.Add("bob", *bob.handle));
  ASSERT_TRUE(registry.Detach("ada"));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  ASSERT_TRUE(registry.Detach("bob"));

  ASSERT_TRUE(ExpiredEventually(expired, 1));
  {
    const std::lock_guard<std::mutex> lock(order_mutex);
    EXPECT_EQ(order, (std::vector<std::string>{"ada"}));  // bob still has 400ms
  }
  EXPECT_EQ(registry.size(), 1U);
  ASSERT_TRUE(ExpiredEventually(expired, 2));
  {
    const std::lock_guard<std::mutex> lock(order_mutex);
    EXPECT_EQ(order, (std::vector<std::string>{"ada", "bob"}));
  }
  EXPECT_EQ(registry.size(), 0U);
}

TEST(SessionRegistryGraceTest, RetentionSkipsThePreLossBacklogAndThePolicy) {
  // Two retention edges: a queue the old wire's failure already cleared is
  // NOT resurrected (pre-loss backlog stays lost — snapshot replay covers
  // it), and overflow while detached never runs the slow-consumer policy.
  std::atomic<int> policy_runs{0};
  Registry::Options options;
  options.grace_period = std::chrono::seconds{300};
  options.queue_while_detached = true;
  options.queue_capacity = 2;
  options.on_slow_consumer = [&policy_runs](const std::string&) { ++policy_runs; };
  Registry registry(std::move(options));
  Session session = MakeSession();
  ASSERT_TRUE(registry.Add("ada", *session.handle));
  ASSERT_TRUE(registry.SendTo("ada", Note{"delivered"}));
  EXPECT_EQ(NextAt(session), "delivered");

  session.client->Close();  // the wire dies mid-session
  ASSERT_TRUE(registry.SendTo("ada", Note{"pre-loss"}));
  ASSERT_TRUE(RefusedEventually(registry, "ada"));  // the writer noticed; queue cleared
  const int policy_before = policy_runs.load();

  ASSERT_TRUE(registry.Detach("ada"));
  ASSERT_TRUE(registry.SendTo("ada", Note{"post-0"}));
  ASSERT_TRUE(registry.SendTo("ada", Note{"post-1"}));
  EXPECT_FALSE(registry.SendTo("ada", Note{"overflow"}));  // full: dropped outright
  EXPECT_EQ(policy_runs.load(), policy_before);            // no policy while detached

  Session fresh = MakeSession();
  ASSERT_TRUE(registry.Resume("ada", *fresh.handle));
  EXPECT_EQ(NextAt(fresh), "post-0");  // pre-loss backlog stayed lost
  EXPECT_EQ(NextAt(fresh), "post-1");
  EXPECT_TRUE(registry.SendTo("ada", Note{"flowing"}));
  EXPECT_EQ(NextAt(fresh), "flowing");
}

}  // namespace
}  // namespace opal::server
