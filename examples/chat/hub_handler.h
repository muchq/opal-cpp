#ifndef OPAL_EXAMPLES_CHAT_HUB_HANDLER_H_
#define OPAL_EXAMPLES_CHAT_HUB_HANDLER_H_

// The multi-client hub (issue #112): the consumer pattern the Go-style
// WebSocket hub hand-rolls, built on the two runtime primitives that make
// it safe and short — owning session handles (EventStream::Share) and the
// queued fan-out registry (opal::server::SessionRegistry). Every session
// registers its handle under a SessionKey id; room traffic fans out through
// the registry's bounded per-session queues, so one slow client never
// stalls a room (the default policy disconnects it instead); and shutdown
// is Drain(): close every session, wait for the handlers to unwind.
//
// Both e2e suites drive this one handler — hub_e2e_test.cc through the
// in-memory pair, hub_cli_test.sh as real processes over real WebSockets
// (hub_server_main.cc + hub_client_main.cc driven by shell commands).
//
// Room membership stays application-level on purpose (the issue's
// non-goals): it is exactly the SessionKey scheme, nothing more. A
// production service would validate names (a nickname containing '/' or a
// leading '#' would confuse this toy scheme) the way it validates any
// input.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "example/chat/server.h"
#include "opal/core/outcome.h"
#include "opal/server/session_registry.h"

namespace example::chat {

// The app-level session-id scheme, in one place: "<room>/<name>", with
// watcher names spelled "#watch-<n>" so occupancy can tell them apart.
struct SessionKey {
  std::string room;
  std::string name;

  bool watcher() const { return name.starts_with('#'); }
  std::string Id() const { return room + "/" + name; }
  // Everything in this key's room, watchers included.
  std::string RoomPrefix() const { return room + "/"; }

  static SessionKey Parse(const std::string& id) {
    const std::size_t slash = id.find('/');
    if (slash == std::string::npos) return {.room = id, .name = ""};
    return {.room = id.substr(0, slash), .name = id.substr(slash + 1)};
  }
};

class HubHandler final : public ChatHandler {
 public:
  using Registry = opal::server::SessionRegistry<RoomEvents>;

  HubHandler() = default;
  explicit HubHandler(Registry::Options options) : registry_(std::move(options)) {}

  // The unary neighbor reports live occupancy straight from the registry:
  // one converse member per id (watchers observe without counting).
  opal::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                           const opal::server::RequestContext&) override {
    std::map<std::string, int> occupancy;
    for (const std::string& id : registry_.Ids()) {
      const SessionKey key = SessionKey::Parse(id);
      if (!key.watcher()) ++occupancy[key.room];
    }
    ListRoomsOutput output;
    for (const auto& [room, members] : occupancy) {
      output.rooms.push_back(RoomSummary{.name = room, .members = members});
    }
    return output;
  }

  opal::Outcome<opal::Unit> Converse(const ConverseInput& input, ConverseServerStream& stream,
                                     const opal::server::RequestContext&) override {
    const SessionKey key{.room = input.room, .name = input.nickname.value_or("anonymous")};
    const std::string id = key.Id();

    // The owning handle replaces the borrowed `stream&` in the registry —
    // Add's atomicity doubles as the nickname reservation.
    if (!registry_.Add(id, stream.Share())) {
      const std::string reason = "nickname '" + key.name + "' is already in " + key.room;
      opal::Error taken = opal::Error::Modeled("Kicked", reason);
      taken.set_detail(Kicked{.message = reason, .by = "hub"});
      return taken;  // one typed exception message, then the close
    }

    registry_.Broadcast(RoomIds(key), RoomEvents::FromJoined(MemberJoined{.member = key.name}));

    bool left_cleanly = false;
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) break;  // wire failed / client vanished
      if ((*event)->is_leave()) {
        left_cleanly = true;
        break;
      }
      if (!(*event)->is_message()) continue;
      // Per-recipient construction — the redaction seam: the author reads
      // "you", everyone else reads the author's name.
      const std::string text = (*event)->as_message().text;
      registry_.Broadcast(RoomIds(key), [&](const std::string& member) {
        ChatMessage view;
        view.text = text;
        view.sender = member == id ? "you" : key.name;
        return RoomEvents::FromMessage(view);
      });
    }

    // Deregister before announcing, so the departure fans out to the others
    // only (Remove would discard this session's own copy anyway); the clean
    // leaver still gets its goodbye directly, on the stream it is draining.
    registry_.Remove(id);
    registry_.Broadcast(RoomIds(key), RoomEvents::FromLeft(MemberLeft{.member = key.name}));
    if (left_cleanly) (void)stream.Send(RoomEvents::FromLeft(MemberLeft{.member = key.name}));
    return opal::Unit{};  // the generated caller closes the session
  }

  opal::Outcome<opal::Unit> Watch(const WatchInput& input, WatchServerStream& stream,
                                  const opal::server::RequestContext&) override {
    // Watchers hold the same handle type as conversers (both directions
    // transmit RoomEvents), so one registry fans out to both.
    const SessionKey key{.room = input.room, .name = "#watch-" + std::to_string(++next_watcher_)};
    (void)registry_.Add(key.Id(), stream.Share());
    // Watchers only listen: this returns at the client's close (nullopt), a
    // wire failure, or the hub closing the session (drain / slow-consumer).
    (void)stream.Receive();
    registry_.Remove(key.Id());
    return opal::Unit{};
  }

  // The graceful-shutdown step (issue #112 proposal 3), for main() to run
  // before the transport's abort-flavored Stop().
  bool Drain(std::chrono::milliseconds grace) { return registry_.Drain(grace); }

  std::size_t sessions() const { return registry_.size(); }

 private:
  // A prefix scan of the whole registry keeps the example honest about what
  // the registry provides; a hub with many busy rooms would keep its own
  // room → members index beside it and skip the scan.
  std::vector<std::string> RoomIds(const SessionKey& key) const {
    const std::string prefix = key.RoomPrefix();
    std::vector<std::string> ids;
    for (std::string& id : registry_.Ids()) {
      if (id.starts_with(prefix)) ids.push_back(std::move(id));
    }
    return ids;
  }

  Registry registry_;
  std::atomic<int> next_watcher_{0};
};

}  // namespace example::chat

#endif  // OPAL_EXAMPLES_CHAT_HUB_HANDLER_H_
