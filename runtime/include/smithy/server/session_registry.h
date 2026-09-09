#ifndef SMITHY_SERVER_SESSION_REGISTRY_H_
#define SMITHY_SERVER_SESSION_REGISTRY_H_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/eventstream/event_stream.h"

namespace opal::server {

// The multi-client fan-out helper (issue #112): a thread-safe map of owning
// session handles (EventStream::Share) with a bounded outbound queue per
// session, so "N connected players, server pushes state to all of them"
// stops being ~200 hand-rolled lines of borrowed references and blocking
// send loops per consumer. Tx is the event union the sessions transmit; Id
// is the application's session key (player id, connection id).
//
//   opal::server::SessionRegistry<RoomEvents> registry;
//   registry.Add(player_id, stream.Share());              // handler entry
//   registry.SendTo(player_id, event);                    // queued, non-blocking
//   registry.Broadcast(ids, [&](const Id& id) { return RedactFor(id); });
//   registry.CloseAll();                                  // the drain recipe, once
//   ...
//   registry.Remove(player_id);                           // handler exit
//
// The queue is the load-bearing decision: SendTo/Broadcast enqueue and
// return, and a per-session writer thread (started by Add) delivers in FIFO
// order — so a broadcast never stalls the room behind the slowest client's
// TCP window. When an attached session's queue is full the event is dropped
// and the slow-consumer policy runs: by default the session is closed (the
// Go-hub answer — its handler observes the close, returns, and removes
// itself); Options::on_slow_consumer replaces that default, keeping policy
// with the application. (A detached session's overflow just drops — see
// Options::queue_while_detached.) Per-recipient construction (the Broadcast(ids, make)
// overloads) exists because broadcast-identical-bytes is the wrong
// primitive for per-viewer state: make runs once per recipient, outside all
// registry locks.
//
// Lifecycle: Add on handler entry, Remove on handler exit — but unlike the
// borrowed-reference registry this replaces, a late Remove is a soft bug
// (stale handles fail with Error::Transport; nothing dangles). Remove never
// closes the session and discards its undelivered events; a writer mid-Send
// finishes or fails that event first, then exits. CloseAll closes every
// registered session; each blocked handler wakes, returns, and Removes
// itself, which is why Drain (expire detached ghosts, CloseAll, then wait
// until the map empties) is the graceful-shutdown step to run before a
// transport Stop() — Stop()
// aborts live sessions rather than draining them (ADR-0015). The registry's
// destructor closes every remaining session and joins every writer, so a
// registry can never outlive-crash its threads.
//
// Reconnect grace (ADR-0020, issue #116): with Options::grace_period set,
// Detach(id) is the abrupt-loss exit path — the entry stays registered
// (size()/Ids() include it), delivery stops, a deadline arms — and
// Resume(id, handle) is the identity-keyed atomic swap a reconnect
// performs within grace. Expiry runs Options::on_expired exactly once,
// mutually exclusive with a successful Resume; Drain and the destructor
// expire detached entries immediately. One lazy expiry thread serves the
// whole registry — a detached session itself holds zero threads.
// ResumeOrAdd is the blessed admission call (ADR-0022): resume-or-fresh-
// join with the brief retry the reconnect race needs, and Close(id) is
// the kick primitive that makes its refusal actionable.
//
// All methods are safe from any thread, including a handler's own (a
// handler may SendTo itself) and the slow-consumer callback (which runs
// with no registry locks held — it may call back into the registry, but
// keep it quick: it runs on whichever thread hit the full queue).
template <typename Tx, typename Id = std::string>
class SessionRegistry {
 public:
  // The value handle (EventStream::Share); the registry keeps its own copy.
  using Handle = eventstream::EventStreamHandle<Tx>;

  struct Options {
    // Bound of each session's outbound queue — the burst a client may fall
    // behind by before the slow-consumer policy runs. Values below 1 mean 1.
    std::size_t queue_capacity = 64;
    // Runs instead of the default close-on-full when a session's queue is
    // full (the event is dropped either way). No registry locks are held.
    std::function<void(const Id&)> on_slow_consumer;
    // Completion-driven delivery (ADR-0019): instead of one writer thread
    // per session, each session's queue drains through a chain of
    // EventStreamHandle::SendAsync completions on the transport's own
    // threads — same FIFO order, same slow-consumer policy, same
    // Remove/Drain/teardown contracts, zero registry threads. Applies per
    // session: one whose socket reports SupportsAsync() false falls back
    // to a writer thread, so mixed fleets keep working at yesterday's
    // cost. Chain steps run on completion contexts — a Beast io thread —
    // so on_slow_consumer stays quick there too.
    //
    // Interplay with direct sends: a writer thread waits out an
    // application send on the session (a handle Send, a co_await Send);
    // the chain cannot wait, so on the first collision the session falls
    // back to a writer thread — nothing is lost or stalled, and that one
    // session thereafter delivers at the writer's one-thread cost while
    // the rest of the fleet stays thread-free. Steady-state pushes to a
    // registered session belong in the registry (SendTo/Broadcast);
    // direct sends are for a session's own request/reply moments.
    bool async_delivery = false;
    // Reconnect grace (ADR-0020, issue #116). Zero keeps Detach/Resume
    // disabled and the registry byte-identical to its prior behavior.
    // With a grace period, Detach(id) parks a dropped session's entry —
    // zero per-session threads — and Resume(id, handle) is the
    // identity-keyed atomic swap a reconnect performs within grace.
    std::chrono::seconds grace_period{0};
    // Runs exactly once when a detached session's grace expires — the
    // cleanup a hand-rolled disconnect timer used to do (collect the
    // game, tell the room). Mutually exclusive with a successful Resume.
    // No registry locks are held; it may call back into the registry.
    // Also runs for detached entries that Drain or the destructor expire
    // immediately (a draining server does not wait out ghosts). It runs
    // off the handler threads: on the registry's expiry thread, or on
    // whichever thread called Drain or the destructor. An exception it
    // throws is swallowed (there is no caller to land in).
    std::function<void(const Id&)> on_expired;
    // Events sent to a detached id are DROPPED by default (reported
    // unqueued): the blessed recovery is snapshot replay on resume, which
    // supersedes anything missed. True retains them in the existing
    // bounded queue instead, delivered after Resume; a full queue drops
    // the event outright — the slow-consumer policy needs a live session
    // to act on. Grace never buys unbounded retention.
    bool queue_while_detached = false;
  };

  SessionRegistry() : SessionRegistry(Options{}) {}
  explicit SessionRegistry(Options options) : options_(Normalize(std::move(options))) {}

  // Closes every remaining session and joins every writer thread (and the
  // expiry thread, if grace ever armed it). Detached entries are expired
  // immediately — on_expired runs for each, still exactly once. Sessions
  // still registered here were leaked by their handlers (Remove-on-exit);
  // closing them is what lets the join terminate.
  ~SessionRegistry() {
    StopExpiry();
    ExpireDetachedNow();
    std::vector<std::shared_ptr<Entry>> all;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [id, entry] : sessions_) all.push_back(std::move(entry));
      sessions_.clear();
      all.insert(all.end(), std::make_move_iterator(retired_.begin()),
                 std::make_move_iterator(retired_.end()));
      retired_.clear();
    }
    for (const auto& entry : all) {
      entry->handle.Close();  // unblocks a writer mid-Send
      RequestStop(*entry);
    }
    for (const auto& entry : all) {
      if (entry->writer.joinable()) entry->writer.join();
    }
  }

  SessionRegistry(const SessionRegistry&) = delete;
  SessionRegistry& operator=(const SessionRegistry&) = delete;

  // Registers a session under id and starts its delivery — a writer
  // thread, or with Options::async_delivery on an async-capable socket, a
  // completion chain armed by the first enqueue. False (and nothing
  // changes — in particular the handle is not closed) when id is already
  // registered. A reconnect under the same id wants ResumeOrAdd; evicting
  // a live id wants Close, so the old handler observes the end and
  // unwinds itself — never Remove-then-Add, which leaves the old handler
  // to tear down the NEW entry on its way out.
  bool Add(Id id, Handle handle) {
    const bool async_mode = options_.async_delivery && handle.SupportsAsync();
    auto entry = std::make_shared<Entry>(std::move(handle));
    entry->async_mode = async_mode;
    const std::lock_guard<std::mutex> lock(mutex_);
    ReapLocked();
    if (!sessions_.emplace(std::move(id), entry).second) return false;
    // Async entries drain through completion chains and hold no thread;
    // the writer starts under the lock, so nothing can observe (or retire)
    // an entry whose writer member is not yet set.
    if (!async_mode) {
      entry->writer = std::thread([entry] { WriterLoop(*entry); });
    }
    return true;
  }

  // Deregisters id: its undelivered events are discarded and its writer
  // exits (after finishing any event mid-Send). Remove never closes the
  // session — the handler that owns the stream usually calls this on its
  // way out, when the session is already over; closing a live session is
  // CloseAll's or the handle's job. On a detached entry Remove is the
  // deliberate-leave path: it cancels the pending expiry, and on_expired
  // never runs. False when id is not registered.
  bool Remove(const Id& id) {
    std::shared_ptr<Entry> entry;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      entry = std::move(it->second);
      sessions_.erase(it);
      // Stop and decide retirement under ONE entry-lock hold: a chain
      // callback converting this entry to writer mode (PumpAsync's
      // collision fallback) either spawned first — and the writer is
      // joinable here, so it retires and gets its join — or observes
      // stopping and never spawns. No interleaving orphans a thread.
      // Entries without a writer need no join; an in-flight chain owns
      // its own shared_ptr.
      bool has_writer = false;
      {
        const std::lock_guard<std::mutex> entry_lock(entry->mutex);
        entry->stopping = true;
        entry->queue.clear();
        has_writer = entry->writer.joinable();
      }
      entry->wake.notify_all();
      if (has_writer) retired_.push_back(entry);
      ReapLocked();
    }
    drained_.notify_all();
    return true;
  }

  // The abrupt-loss exit path (ADR-0020): instead of Remove, park the
  // entry detached — still registered, delivery stopped, deadline armed,
  // zero per-session threads (the writer exits, its join deferred to
  // Resume or expiry; a chain goes idle). The session's old handle is
  // closed (idempotent on a wire that already died — the usual reason to
  // be here) so nothing stays wedged mid-send. False for an unknown or
  // already-detached id, or when Options::grace_period is zero.
  bool Detach(const Id& id) {
    if (options_.grace_period == std::chrono::seconds{0}) return false;
    std::shared_ptr<Entry> entry;
    std::optional<Handle> old_handle;  // a copy to close outside the locks
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      entry = it->second;
      {
        const std::lock_guard<std::mutex> entry_lock(entry->mutex);
        if (entry->detached) return false;
        entry->detached = true;
        entry->deadline = std::chrono::steady_clock::now() + options_.grace_period;
        entry->stopping = true;  // delivery stops; Resume clears this
        if (!options_.queue_while_detached) entry->queue.clear();
        old_handle.emplace(entry->handle);
      }
      entry->wake.notify_all();
      ArmExpiryLocked();
    }
    old_handle->Close();
    return true;
  }

  // The identity-keyed atomic swap a reconnect performs (ADR-0020): only
  // succeeds on a detached entry still within grace — the new handle
  // replaces the old, delivery re-arms (writer or completion chain, per
  // the new handle), and the pending expiry is cancelled. Mutually
  // exclusive with on_expired by construction: expiry claims an entry by
  // removing it under the registry lock, so exactly one of the two wins.
  // False on an attached entry (that id is live — whether to kick it is
  // the application's call), an expired or unknown id, or when grace is
  // disabled. The caller then treats the connection as a fresh join.
  // Resume may block briefly — the old connection's in-flight send
  // failing out — and it holds the registry lock while it waits, so
  // Add/Remove/SendTo/Broadcast and expiry stall behind it for that beat.
  bool Resume(const Id& id, Handle handle) {
    if (options_.grace_period == std::chrono::seconds{0}) return false;
    const bool async_mode = options_.async_delivery && handle.SupportsAsync();
    std::shared_ptr<Entry> entry;
    bool kick = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      entry = it->second;
      std::unique_lock<std::mutex> entry_lock(entry->mutex);
      if (!entry->detached || std::chrono::steady_clock::now() >= entry->deadline) {
        return false;  // live, or expiry owns it (its thread will claim it)
      }
      // Delivery quiesced before the handle swap: the old writer exited
      // (Detach closed its session, so a mid-send writer unblocks
      // promptly) and any chain completion has landed. Bounded-short by
      // construction; both flips notify entry->wake. Held under the
      // registry lock — the whole registry waits with us, which is what
      // forecloses a concurrent expiry claim or Remove mid-swap.
      entry->wake.wait(entry_lock, [&entry] {
        return (!entry->writer.joinable() || entry->done) && !entry->delivering;
      });
      if (entry->writer.joinable()) entry->writer.join();  // instant: done
      entry->handle = std::move(handle);
      entry->detached = false;
      entry->stopping = false;
      entry->done = false;
      entry->async_mode = async_mode;
      if (!async_mode) {
        entry->writer = std::thread([entry] { WriterLoop(*entry); });
      } else if (!entry->queue.empty()) {
        entry->delivering = true;  // retained tail: re-kick below
        kick = true;
      }
    }
    entry->wake.notify_all();
    if (kick) PumpAsync(entry);
    return true;
  }

  // The three-way admission outcome ResumeOrAdd reports — every consumer
  // branches on it: kResumed replays the snapshot (ADR-0020's recovery),
  // kAdded announces the fresh join, kRefused answers the collision.
  enum class Admission { kResumed, kAdded, kRefused };

  // The admission recipe as a primitive (ADR-0022, issue #122): Resume
  // first (the reconnect story), fresh Add second, retried every ~50ms
  // until the deadline — a reconnect can beat the old wire's failure
  // notice, so for a beat the id looks live to both paths and only
  // retrying converges. `mint` runs exactly once per attempt: the one
  // fresh Share() serves both the Resume try and the Add try (handles
  // are cheap-copy views). A zero deadline is the single-shot form; an
  // enormous one (milliseconds::max()) is a legal "block until
  // admitted". This method BLOCKS up to `deadline`: call it before a
  // handler's first suspension — on the launching thread, where brief
  // blocking is fine — never from a completion context. On a
  // grace-disabled registry Resume never succeeds, so the call degrades
  // to a retried Add — still the right admission shape; kResumed simply
  // cannot occur. kRefused at the deadline almost always means the id is
  // live on another connection (rarely: a detached entry past its grace
  // that expiry has not yet claimed — a beat later the same call would
  // report kAdded); whether to kick a live one (Close) is the
  // application's call — ResumeOrAdd never kicks on its own.
  Admission ResumeOrAdd(const Id& id, const std::function<Handle()>& mint,
                        std::chrono::milliseconds deadline) {
    constexpr auto kRetryInterval = std::chrono::milliseconds(50);
    // Clamped, not added blindly: now + milliseconds::max() overflows the
    // steady clock's representation (UB, and a wrapped deadline would
    // silently degrade "block until admitted" to single-shot).
    const auto start = std::chrono::steady_clock::now();
    const auto headroom = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - start);
    const auto give_up =
        deadline >= headroom ? std::chrono::steady_clock::time_point::max() : start + deadline;
    while (true) {
      Handle handle = mint();
      if (Resume(id, handle)) return Admission::kResumed;
      if (Add(id, std::move(handle))) return Admission::kAdded;
      const auto now = std::chrono::steady_clock::now();
      if (now >= give_up) return Admission::kRefused;
      std::this_thread::sleep_for(
          std::min<std::chrono::steady_clock::duration>(kRetryInterval, give_up - now));
    }
  }

  // Queues one event for id; delivery is FIFO whichever mode the session
  // runs in. True when queued. False — the event is dropped — for an
  // unknown id; a session whose delivery already failed; an attached
  // session's full queue (after running the slow-consumer policy); or a
  // detached id, unless Options::queue_while_detached retains it (a full
  // detached queue drops with no policy run — see the Option).
  bool SendTo(const Id& id, Tx event) {
    std::shared_ptr<Entry> entry;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      entry = it->second;
    }
    return Enqueue(id, entry, std::move(event));
  }

  // Queues make(id)'s event for each registered id in ids (unknown ids are
  // skipped; make runs only for registered ones, outside all registry
  // locks). Returns how many were queued.
  std::size_t Broadcast(const std::vector<Id>& ids, const std::function<Tx(const Id&)>& make) {
    // Borrow the caller's ids rather than copying them; they outlive the call.
    std::vector<std::pair<const Id*, std::shared_ptr<Entry>>> targets;
    targets.reserve(ids.size());
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const Id& id : ids) {
        if (const auto it = sessions_.find(id); it != sessions_.end()) {
          targets.emplace_back(&id, it->second);
        }
      }
    }
    std::size_t queued = 0;
    for (auto& [id, entry] : targets) {
      if (Enqueue(*id, entry, make(*id))) ++queued;
    }
    return queued;
  }

  // The identical-bytes convenience of the above.
  std::size_t Broadcast(const std::vector<Id>& ids, const Tx& event) {
    return Broadcast(ids, [&event](const Id&) { return event; });
  }

  // Broadcast to every currently registered session (one registry pass, no
  // intermediate Ids() snapshot).
  std::size_t Broadcast(const std::function<Tx(const Id&)>& make) {
    std::vector<std::pair<Id, std::shared_ptr<Entry>>> targets;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      targets.reserve(sessions_.size());
      for (const auto& [id, entry] : sessions_) targets.emplace_back(id, entry);
    }
    std::size_t queued = 0;
    for (auto& [id, entry] : targets) {
      if (Enqueue(id, entry, make(id))) ++queued;
    }
    return queued;
  }
  std::size_t Broadcast(const Tx& event) {
    return Broadcast([&event](const Id&) { return event; });
  }

  // Closes id's current session — the kick primitive (ADR-0022): the
  // session observes the close and its handler runs its normal exit path,
  // making the id admittable on the next dial — freed outright on a
  // Remove exit; parked-resumable on a Detach exit, where the next
  // ResumeOrAdd reports kResumed and snapshot replay carries the old
  // session's identity across. Policy stays
  // with the application: this is how a ResumeOrAdd refusal becomes
  // actionable when the old session is known dead (a silent partition, an
  // operator action) instead of waiting out TCP. On a detached entry it
  // closes an already-closed handle — a harmless no-op (detached ids are
  // Resume's and Remove's business). False for an unknown id. The handle
  // copy is taken under the entry lock (the Resume-swap discipline) and
  // closed outside all locks.
  bool Close(const Id& id) {
    std::optional<Handle> doomed;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(id);
      if (it == sessions_.end()) return false;
      const std::lock_guard<std::mutex> entry_lock(it->second->mutex);
      doomed.emplace(it->second->handle);
    }
    doomed->Close();
    return true;
  }

  // Closes every registered session (idempotent, non-blocking): each
  // blocked handler wakes and returns, unregistering itself on the way out.
  void CloseAll() {
    // Handle COPIES, taken under the lock Resume swaps under — closing
    // through the entries after releasing it would race a concurrent
    // Resume's handle swap. The closes still run outside every lock.
    std::vector<Handle> handles;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handles.reserve(sessions_.size());
      for (const auto& [id, entry] : sessions_) handles.push_back(entry->handle);
    }
    for (Handle& handle : handles) handle.Close();
  }

  // The graceful-shutdown step (issue #112 proposal 3): CloseAll, then wait
  // for the handlers to Remove themselves. True once the registry is empty;
  // false on timeout (some handler is stuck or forgot Remove) — after
  // which the transport's Stop() aborts whatever is left, per ADR-0015.
  bool Drain(std::chrono::milliseconds timeout) {
    // A draining server does not wait out ghosts: detached entries are
    // expired now (on_expired runs for each, still exactly once).
    ExpireDetachedNow();
    CloseAll();
    std::unique_lock<std::mutex> lock(mutex_);
    return drained_.wait_for(lock, timeout, [this] { return sessions_.empty(); });
  }

  // A snapshot of the registered ids — the "everyone currently here" input
  // to the Broadcast(ids, ...) overloads.
  std::vector<Id> Ids() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Id> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, entry] : sessions_) ids.push_back(id);
    return ids;
  }

  std::size_t size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
  }

 private:
  struct Entry {
    explicit Entry(Handle h) : handle(std::move(h)) {}
    // Written only by Resume, under BOTH the registry and entry locks,
    // after delivery quiesces (the writer exited and no chain completion
    // is in flight). Delivery reads it lock-free — the quiescence wait is
    // what makes that safe — and every other reader (CloseAll, the
    // close-on-full default) copies it under one of those locks and acts
    // on the copy.
    Handle handle;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Tx> queue;
    bool stopping = false;  // delivery must end: Remove/teardown/Detach
                            // asked, or its own delivery failed (queue
                            // discarded unless detached and retaining)
    bool done = false;      // the writer exited; join is now instant
    // Set in Add before the entry is visible; thereafter changed only
    // under the entry lock (the chain->writer collision fallback,
    // Resume's re-arm from the new handle).
    bool async_mode = false;
    bool delivering = false;  // an async send chain is in flight
    // ADR-0020: parked awaiting Resume or expiry. While detached,
    // stopping is also true (delivery is down); Resume clears both.
    bool detached = false;
    std::chrono::steady_clock::time_point deadline;  // meaningful iff detached
    std::thread writer;                              // sync mode only: started by Add, joined by
                                                     // Reap/destructor (async entries hold none)
  };

  static Options Normalize(Options options) {
    if (options.queue_capacity < 1) options.queue_capacity = 1;
    if (options.grace_period < std::chrono::seconds{0}) {
      options.grace_period = std::chrono::seconds{0};
    }
    return options;
  }

  // The per-session delivery loop: FIFO off the queue, one blocking Send at
  // a time. A failed Send is terminal for delivery (the session is closed
  // or gone) — remaining events are discarded (unless the entry is
  // detached and retaining its queue for a resume) and the writer exits;
  // the handler side observes the same failure through its own stream.
  static void WriterLoop(Entry& entry) {
    for (;;) {
      Tx event;
      {
        std::unique_lock<std::mutex> lock(entry.mutex);
        entry.wake.wait(lock, [&entry] { return entry.stopping || !entry.queue.empty(); });
        if (entry.stopping) break;
        event = std::move(entry.queue.front());
        entry.queue.pop_front();
      }
      if (!entry.handle.Send(event).ok()) {
        const std::lock_guard<std::mutex> lock(entry.mutex);
        entry.stopping = true;
        // A detached entry may be retaining its queue for the resume
        // (the failed send is the dying old connection's, not the tail's).
        if (!entry.detached) entry.queue.clear();
        break;
      }
    }
    {
      const std::lock_guard<std::mutex> lock(entry.mutex);
      entry.done = true;
    }
    entry.wake.notify_all();  // Resume waits out this exit before swapping
  }

  // Ask the writer to exit, discarding undelivered events. (The writer's
  // own failure path sets the same state inline, under its held lock.)
  static void RequestStop(Entry& entry) {
    {
      const std::lock_guard<std::mutex> lock(entry.mutex);
      entry.stopping = true;
      entry.queue.clear();
    }
    entry.wake.notify_all();
  }

  bool Enqueue(const Id& id, const std::shared_ptr<Entry>& entry, Tx event) {
    bool queued = false;
    bool kick = false;
    {
      const std::lock_guard<std::mutex> lock(entry->mutex);
      if (entry->detached) {
        // Tested before stopping: detached implies stopping, and the
        // retention branch must win over the refuse-on-stopping one.
        // Default: drop — snapshot replay on resume is authoritative.
        // Opt-in retention queues to capacity; a full queue drops outright
        // (the slow-consumer policy needs a live session to act on).
        if (!options_.queue_while_detached) return false;
        if (entry->queue.size() >= options_.queue_capacity) return false;
        entry->queue.push_back(std::move(event));
        return true;  // delivered after Resume re-arms delivery
      }
      if (entry->stopping) return false;
      if (entry->queue.size() < options_.queue_capacity) {
        entry->queue.push_back(std::move(event));
        queued = true;
        if (entry->async_mode && !entry->delivering) {
          entry->delivering = true;
          kick = true;
        }
      }
    }
    if (queued) {
      if (kick) {
        PumpAsync(entry);  // outside the entry lock, like every send
      } else if (!entry->async_mode) {
        entry->wake.notify_one();  // outside the lock: the writer wakes runnable
      }
      return true;
    }
    // Full queue: drop the event, let policy decide the session's fate —
    // with no locks held, so the callback may call back into the registry.
    if (options_.on_slow_consumer) {
      options_.on_slow_consumer(id);
    } else {
      // A handle copy under the entry lock (which Resume's swap holds),
      // closed outside it — closing through the entry directly would race
      // a concurrent Resume's swap. The copy is whichever connection the
      // id has as the drop is charged; that is close-on-full's contract.
      std::optional<Handle> doomed;
      {
        const std::lock_guard<std::mutex> lock(entry->mutex);
        doomed.emplace(entry->handle);
      }
      doomed->Close();
    }
    return false;
  }

  // The async delivery chain (ADR-0019): one event in flight per session,
  // each completion sending the next — FIFO like the writer it replaces.
  // The in-flight event stays at the queue's front until its completion
  // reports success, so a refused send loses nothing. Outcomes:
  // Error::Transport is terminal exactly as in WriterLoop (the session is
  // closed or gone); Error::Validation means the socket's one send slot
  // is held by an application send, and converts the session to
  // writer-thread delivery (the collision fallback — see the callback
  // below). An event the encoder refuses converts the same way and then
  // kills delivery in the writer, WriterLoop's terminal semantics — an
  // encoder-refusable event in a fan-out union is an application bug.
  // Chain steps run on the transport's completion context; the in-memory
  // pair's ready path completes inline on the enqueuer, one frame per
  // drained event, so recursion depth tracks the drain — the wire
  // transports post completions and stay at depth one.
  static void PumpAsync(const std::shared_ptr<Entry>& entry) {
    Tx event;
    {
      const std::lock_guard<std::mutex> lock(entry->mutex);
      if (entry->stopping || entry->queue.empty()) {
        entry->delivering = false;
        entry->wake.notify_all();  // Resume waits out the chain's idle flip
        return;
      }
      event = entry->queue.front();  // copy: the slot is the in-flight marker
    }
    entry->handle.SendAsync(event, [entry](const Outcome<Unit>& sent) {
      bool next = false;
      {
        const std::lock_guard<std::mutex> lock(entry->mutex);
        if (sent.ok()) {
          // Delivered: retire the event. (RequestStop may have cleared the
          // queue mid-flight, so the pop is guarded.)
          if (!entry->queue.empty()) entry->queue.pop_front();
          next = true;
        } else if (sent.error().kind() == ErrorKind::kValidation) {
          // Refused, not failed: an application send holds the session's
          // one send slot (websocket.h's one-outstanding contract) — and
          // that includes the tail of a coroutine's own completed send
          // whose completion bookkeeping hasn't landed yet. The chain
          // cannot wait and sparse traffic may never enqueue again, so
          // the session falls back to what CAN wait: its own writer
          // thread, spawned once, delivering this queue with the blocking
          // serialize-by-waiting semantics. A session that mixes direct
          // sends with registry delivery pays one thread; pure chain
          // sessions stay at zero.
          entry->delivering = false;
          if (!entry->stopping && !entry->writer.joinable()) {
            entry->async_mode = false;
            entry->writer = std::thread([entry] { WriterLoop(*entry); });
          }
        } else {
          entry->stopping = true;  // delivery failed; the session is over
          // A detached entry may be retaining its queue for the resume —
          // this failure is the dying old connection's, not the tail's.
          if (!entry->detached) entry->queue.clear();
          entry->delivering = false;
        }
      }
      if (next) {
        PumpAsync(entry);
      } else {
        entry->wake.notify_all();  // Resume waits out the chain's last flip
      }
    });
  }

  // Joins retired writers that have finished (done == true, so the join is
  // instant); called under mutex_ from Add/Remove so a churny hub never
  // accumulates unjoined threads. Writers still delivering stay retired
  // until a later reap or the destructor.
  void ReapLocked() {
    auto it = retired_.begin();
    while (it != retired_.end()) {
      if (!(*it)->writer.joinable()) {
        // Defensive only — Remove retires writer entries exclusively, and
        // a started writer stays joinable until reaped.
        it = retired_.erase(it);
        continue;
      }
      bool done = false;
      {
        const std::lock_guard<std::mutex> lock((*it)->mutex);
        done = (*it)->done;
      }
      if (done) {
        (*it)->writer.join();
        it = retired_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // --- Grace expiry (ADR-0020) --------------------------------------
  // One lazy thread for the whole registry: started by the first Detach,
  // parked until the nearest deadline, joined by the destructor. Expiry
  // CLAIMS an entry by erasing it under the registry lock — the same lock
  // Resume swaps under — which is what makes on_expired and a successful
  // Resume mutually exclusive, exactly once, by construction.

  // Registry lock held.
  void ArmExpiryLocked() {
    if (!expiry_.joinable()) {
      expiry_ = std::thread([this] { ExpiryLoop(); });
    }
    expiry_wake_.notify_all();  // a new deadline may now be the nearest
  }

  void StopExpiry() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      expiry_stop_ = true;
    }
    expiry_wake_.notify_all();
    if (expiry_.joinable()) expiry_.join();
  }

  void ExpiryLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!expiry_stop_) {
      std::optional<std::chrono::steady_clock::time_point> next;
      for (const auto& [id, entry] : sessions_) {
        const std::lock_guard<std::mutex> entry_lock(entry->mutex);
        if (!entry->detached) continue;
        if (!next || entry->deadline < *next) next = entry->deadline;
      }
      if (!next) {
        expiry_wake_.wait(lock);
        continue;
      }
      expiry_wake_.wait_until(lock, *next);
      if (expiry_stop_) break;
      auto expired = TakeExpiredLocked(std::chrono::steady_clock::now());
      if (expired.empty()) continue;
      lock.unlock();
      FireExpiries(expired);
      lock.lock();
    }
  }

  // Registry lock held: claim every detached entry whose deadline passed
  // (erase from the map; retire exited writers for their join) and return
  // the claims for the caller to fire outside all locks. One entry-lock
  // hold decides both claim and writer: a claimable entry is detached,
  // hence stopping, so the chain fallback's spawn (guarded on !stopping)
  // cannot make the writer joinable after we look — and joins run only
  // under mutex_, which we hold.
  std::vector<std::pair<Id, std::shared_ptr<Entry>>> TakeExpiredLocked(
      std::chrono::steady_clock::time_point now) {
    std::vector<std::pair<Id, std::shared_ptr<Entry>>> expired;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      bool claim = false;
      bool has_writer = false;
      {
        const std::lock_guard<std::mutex> entry_lock(it->second->mutex);
        claim = it->second->detached && now >= it->second->deadline;
        has_writer = claim && it->second->writer.joinable();
      }
      if (!claim) {
        ++it;
        continue;
      }
      if (has_writer) retired_.push_back(it->second);
      expired.emplace_back(it->first, std::move(it->second));
      it = sessions_.erase(it);
    }
    if (!expired.empty()) ReapLocked();
    return expired;
  }

  void FireExpiries(const std::vector<std::pair<Id, std::shared_ptr<Entry>>>& expired) {
    for (const auto& [id, entry] : expired) {
      if (!options_.on_expired) continue;
      try {
        options_.on_expired(id);
      } catch (...) {
        // On the expiry thread (or mid-teardown) there is no caller to
        // rethrow to — an escaping exception would terminate the process
        // and starve the rest of the batch. A throwing expiry policy is
        // the application's bug; the claim already happened either way.
      }
    }
    drained_.notify_all();  // the map shrank; a Drain may now be done
  }

  // Expire every detached entry immediately, deadline or not — the Drain
  // and destructor path.
  void ExpireDetachedNow() {
    std::vector<std::pair<Id, std::shared_ptr<Entry>>> expired;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      expired = TakeExpiredLocked(std::chrono::steady_clock::time_point::max());
    }
    FireExpiries(expired);
  }

  const Options options_;
  mutable std::mutex mutex_;
  std::condition_variable drained_;
  std::map<Id, std::shared_ptr<Entry>> sessions_;
  std::vector<std::shared_ptr<Entry>> retired_;
  // The grace machinery's expiry thread (see above); guarded by mutex_.
  std::thread expiry_;
  std::condition_variable expiry_wake_;
  bool expiry_stop_ = false;
};

}  // namespace opal::server

#endif  // SMITHY_SERVER_SESSION_REGISTRY_H_
