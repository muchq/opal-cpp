// The thread-free hub on the GENERATED async surface (ADR-0021): the same
// Converse wire async_hub_cli_test.sh has always driven, with zero
// hand-written route matching, input parsing, envelope codecs, or refusal
// framing — ChatAsyncHandler + ChatServer own all of it, and the typed
// Kicked refusal is just a co_returned error the generated wrapper frames.
// Fan-out still rides a SessionRegistry in async delivery mode with
// reconnect grace (ADR-0020): N rooms of players run on the transport's
// fixed io threads plus a handler pool that only launches. (This main was
// the hand-written async mount until the generated path landed; that shape
// remains documented in ADR-0019 and the server guide.)
//
//   bazel run //examples/chat:async_hub_server
//   bazel run //examples/chat:hub_client -- 8080 lobby ada    # per terminal
//   kill -TERM <pid>   # drains the hub, then exits 0

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "example/chat/server.h"
#include "example/chat/types.h"
#include "smithy/core/outcome.h"
#include "smithy/http/beast_transport.h"
#include "smithy/server/session_registry.h"

namespace {

using example::chat::ChatMessage;
using example::chat::ConverseAsyncServerStream;
using example::chat::ConverseInput;
using example::chat::Kicked;
using example::chat::ListRoomsInput;
using example::chat::ListRoomsOutput;
using example::chat::MemberJoined;
using example::chat::MemberLeft;
using example::chat::RoomEvents;
using example::chat::RoomSummary;
using example::chat::WatchAsyncServerStream;
using example::chat::WatchInput;

using Registry = opal::server::SessionRegistry<RoomEvents>;

// The fan-out state: ids are "<room>/<name>", exactly the hub_handler
// scheme, over the async-delivery registry — no writer threads, no parked
// handler threads, all delivery on the transport's completions. Watchers
// (the Watch operation's read-only seats) ride the same registry under
// "#watch-" names, so broadcasts reach them while the roster ignores them.
class AsyncHub {
 public:
  explicit AsyncHub(std::chrono::seconds grace) : registry_(RegistryOptions(this, grace)) {}

  opal::server::SessionRegistry<RoomEvents>& registry() { return registry_; }

  // A watcher seat's id is "<room>/#watch-<n>" — one predicate, one
  // spelling, for every roster/occupancy filter below.
  static bool IsWatcherSeat(const std::string& id, std::size_t slash) {
    return slash + 1 < id.size() && id[slash + 1] == '#';
  }

  // The roster a resumed session replays as its snapshot (ADR-0020's
  // recovery model: authoritative current state, not missed messages).
  // Watcher seats are delivery-only and stay out of it.
  std::vector<std::string> RoomMembers(const std::string& room) const {
    const std::string prefix = room + "/";
    std::vector<std::string> names;
    for (const std::string& id : registry_.Ids()) {
      if (id.starts_with(prefix) && !IsWatcherSeat(id, prefix.size() - 1)) {
        names.push_back(id.substr(prefix.size()));
      }
    }
    return names;
  }

  ListRoomsOutput ListRooms() const {
    std::map<std::string, std::int32_t> counts;
    for (const std::string& id : registry_.Ids()) {
      const auto slash = id.find('/');
      if (slash == std::string::npos || IsWatcherSeat(id, slash)) continue;
      ++counts[id.substr(0, slash)];
    }
    ListRoomsOutput output;
    for (const auto& [name, members] : counts) {
      output.rooms.push_back(RoomSummary{.name = name, .members = members});
    }
    return output;
  }

  void BroadcastToRoom(const std::string& room, const RoomEvents& event) {
    registry_.Broadcast(RoomIds(room), event);
  }

  void BroadcastMessage(const std::string& room, const std::string& author_id,
                        const std::string& author, const std::string& text) {
    registry_.Broadcast(RoomIds(room), [&](const std::string& member) {
      ChatMessage view;
      view.text = text;
      view.sender = member == author_id ? "you" : author;
      return RoomEvents::FromMessage(view);
    });
  }

 private:
  static opal::server::SessionRegistry<RoomEvents>::Options RegistryOptions(
      AsyncHub* hub, std::chrono::seconds grace) {
    opal::server::SessionRegistry<RoomEvents>::Options options;
    options.async_delivery = true;  // ADR-0019: chains, not writer threads
    options.grace_period = grace;   // ADR-0020: abrupt losses detach, not vanish
    options.on_expired = [hub](const std::string& id) {
      // Grace ran out: the departure the disconnect deferred. Runs off
      // the handler threads (the registry's expiry thread, or the Drain
      // caller on shutdown); Broadcast is safe from any thread.
      const auto slash = id.find('/');
      if (slash == std::string::npos) return;
      hub->BroadcastToRoom(id.substr(0, slash),
                           RoomEvents::FromLeft(MemberLeft{.member = id.substr(slash + 1)}));
    };
    return options;
  }

  // Everyone the room's broadcasts reach — members and watcher seats.
  std::vector<std::string> RoomIds(const std::string& room) const {
    const std::string prefix = room + "/";
    std::vector<std::string> ids;
    for (std::string& id : registry_.Ids()) {
      if (id.starts_with(prefix)) ids.push_back(std::move(id));
    }
    return ids;
  }

  opal::server::SessionRegistry<RoomEvents> registry_;
};

// The generated async handler (ADR-0021): each streaming method is one
// session's whole life as a coroutine — launched from the handler pool,
// then living entirely on completion contexts.
class HubHandler final : public example::chat::ChatAsyncHandler {
 public:
  explicit HubHandler(std::chrono::seconds grace) : hub_(grace) {}

  AsyncHub& hub() { return hub_; }

  opal::eventstream::StreamTask Converse(ConverseInput input,
                                         ConverseAsyncServerStream& stream) override {
    const std::string room = input.room;
    const std::string name = input.nickname.value_or("anonymous");
    const std::string id = room + "/" + name;
    // The blessed admission call (ADR-0022): resume-or-fresh-join with the
    // brief retry the reconnect race needs. It blocks, legally: this runs
    // pre-first-suspend, on the launching handler thread.
    const auto admission = hub_.registry().ResumeOrAdd(
        id, [&stream] { return stream.Share(); }, std::chrono::seconds(1));
    if (admission == Registry::Admission::kRefused) {
      // The nickname reservation refused: co_return the modeled error and
      // the generated wrapper frames the typed exception — the code the
      // hand-written mount used to carry.
      Kicked kicked;
      kicked.message = "nickname '" + name + "' is already in " + room;
      kicked.by = "hub";
      auto refusal = opal::Error::Modeled("Kicked", *kicked.message);
      refusal.set_detail(std::move(kicked));
      co_return refusal;
    }

    if (admission == Registry::Admission::kResumed) {
      // Snapshot replay, the blessed recovery: the roster as it stands now,
      // as this session's first events. Direct sends are the handler's
      // request/reply moment; a collision with a broadcast chain just
      // converts this session to writer delivery.
      for (const std::string& member : hub_.RoomMembers(room)) {
        (void)co_await stream.Send(RoomEvents::FromJoined(MemberJoined{.member = member}));
      }
    } else {
      hub_.BroadcastToRoom(room, RoomEvents::FromJoined(MemberJoined{.member = name}));
    }

    bool left_cleanly = false;
    while (true) {
      auto received = co_await stream.Receive();
      if (!received.ok()) break;          // wire failed
      if (!received->has_value()) break;  // client closed without a leave
      if ((*received)->is_leave()) {
        left_cleanly = true;
        break;
      }
      if (!(*received)->is_message()) continue;
      hub_.BroadcastMessage(room, id, name, (*received)->as_message().text);
    }

    if (left_cleanly) {
      hub_.registry().Remove(id);
      hub_.BroadcastToRoom(room, RoomEvents::FromLeft(MemberLeft{.member = name}));
      // Best-effort: a registry chain still draining this session's tail
      // holds the send slot and refuses this — the discard is deliberate.
      (void)co_await stream.Send(RoomEvents::FromLeft(MemberLeft{.member = name}));
    } else if (!hub_.registry().Detach(id)) {
      // Grace disabled (or the entry already gone): the pre-ADR-0020 path.
      hub_.registry().Remove(id);
      hub_.BroadcastToRoom(room, RoomEvents::FromLeft(MemberLeft{.member = name}));
    }
    // An abrupt loss inside grace stays silent: the resumed session replays
    // the roster, and only expiry announces the departure (on_expired).
    co_return opal::Unit{};  // the generated wrapper closes the stream
  }

  opal::eventstream::StreamTask Watch(WatchInput input, WatchAsyncServerStream& stream) override {
    // A read-only seat in the room's fan-out: broadcasts reach it, the
    // roster ignores it, and it gets no reconnect grace (nothing to
    // resume). Rx is NoEvents, so the Receive only ever reports the close.
    const std::string id = input.room + "/#watch-" + std::to_string(watch_seq_.fetch_add(1));
    if (!hub_.registry().Add(id, stream.Share())) {
      co_return opal::Error::Validation("watch seat admission failed");
    }
    (void)co_await stream.Receive();
    hub_.registry().Remove(id);
    co_return opal::Unit{};
  }

  opal::Outcome<ListRoomsOutput> ListRooms(
      const ListRoomsInput& /*input*/, const opal::server::RequestContext& /*context*/) override {
    return hub_.ListRooms();
  }

 private:
  AsyncHub hub_;
  std::atomic<int> watch_seq_{0};
};

}  // namespace

int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  // argv: [port [grace-seconds]] — 0 binds an ephemeral port; the short
  // grace is what lets the CLI test watch an expiry inside its timeout.
  const std::chrono::seconds grace{argc > 2 ? std::atoi(argv[2]) : 300};
  auto handler = std::make_shared<HubHandler>(grace);
  // The async constructor (ADR-0021): every streaming route registers on
  // the shared-session seam; the unary table (ListRooms) is identical.
  example::chat::ChatServer server(handler);

  opal::http::BeastServerTransport::Options options;
  options.address = "0.0.0.0";
  options.port = argc > 1 ? std::atoi(argv[1]) : 8080;  // 0 binds an ephemeral port
  options.handler_threads = 2;  // launch points + unary requests; sessions hold no thread
  options.websocket_gate = server.StreamRouter()->Gate();
  options.on_websocket_session = server.StreamRouter()->ServeSession();
  opal::http::BeastServerTransport transport(options);
  opal::Outcome<opal::Unit> started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "async-hub: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "async-hub: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);
  std::fprintf(stderr, "async-hub: signal %d, draining %zu session(s)\n", signal_number,
               handler->hub().registry().size());
  const bool drained = handler->hub().registry().Drain(std::chrono::seconds(5));
  std::fprintf(stderr, drained ? "async-hub: drained\n"
                               : "async-hub: drain timed out; aborting remaining sessions\n");
  transport.Stop();
  return drained ? 0 : 1;
}
