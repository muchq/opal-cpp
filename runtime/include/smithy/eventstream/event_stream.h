#ifndef SMITHY_EVENTSTREAM_EVENT_STREAM_H_
#define SMITHY_EVENTSTREAM_EVENT_STREAM_H_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "smithy/core/outcome.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace opal::eventstream {

// The vacant direction of a one-directional stream (ADR-0016): when an
// operation models no event union for a direction, generated code
// parameterizes that direction's EventStream slot with NoEvents. A NoEvents
// transmit direction makes Send uncallable (a static_assert catches the
// misuse at compile time), so the encoder slot is never invoked and
// generated code leaves it empty; a message received on a NoEvents
// direction is undecodable and therefore terminal (the generated decoder
// rejects it).
struct NoEvents {
  friend bool operator==(const NoEvents&, const NoEvents&) = default;
};

template <typename Tx, typename Rx>
class AsyncEventStream;  // the coroutine adapter (ADR-0019) also mints handles

namespace internal {

// The seam between a stream and its handles (issue #112): a revocable view
// of the stream's socket. Handles pin the pointer (`active`) around each
// socket call; revocation nulls the pointer so no new operation starts,
// closes the socket so any operation already inside a call unblocks, then
// waits for `active` to drain — after which the socket may safely die with
// the stream's borrow. Handles outlive all of it through this shared state:
// once `socket` is null they fail softly, they never dangle.
struct SharedSessionState {
  std::mutex mutex;
  std::condition_variable idle;
  http::WebSocket* socket = nullptr;  // null once the stream ended
  int active = 0;                     // handle operations inside socket calls
};

// Drops one pin; the revoking stream's drain wait wakes at zero. Free so
// async completions can release with only the state in hand (ADR-0019).
inline void ReleasePin(SharedSessionState& state) {
  const std::lock_guard<std::mutex> lock(state.mutex);
  if (--state.active == 0) state.idle.notify_all();
}

// The stream-side end of the shared view: owns revocation, so EventStream's
// destructor and move operations stay defaulted (no member list to forget
// when the class grows). Move-only; declared as EventStream's last member,
// so it revokes before the socket members it guards are torn down.
class SharedViewOwner {
 public:
  SharedViewOwner() = default;
  ~SharedViewOwner() { End(); }
  SharedViewOwner(SharedViewOwner&& other) noexcept
      : state_(std::move(other.state_)), socket_(std::exchange(other.socket_, nullptr)) {}
  SharedViewOwner& operator=(SharedViewOwner&& other) noexcept {
    if (this != &other) {
      End();
      state_ = std::move(other.state_);
      socket_ = std::exchange(other.socket_, nullptr);
    }
    return *this;
  }
  SharedViewOwner(const SharedViewOwner&) = delete;
  SharedViewOwner& operator=(const SharedViewOwner&) = delete;

  // The state handles share, created on first use (a stream that never
  // shares carries none and End() is a no-op).
  std::shared_ptr<SharedSessionState> Ensure(http::WebSocket* socket) {
    if (state_ == nullptr) {
      state_ = std::make_shared<SharedSessionState>();
      state_->socket = socket;
      socket_ = socket;
    }
    return state_;
  }

  // Revoke-then-drain: null the pointer so no new handle operation starts,
  // close the socket so any operation already inside a call unblocks, wait
  // for the in-flight count to drain. After this returns no handle can
  // reach the socket again, so the borrow (or owned socket) may die.
  void End() {
    if (state_ == nullptr) return;
    const std::shared_ptr<SharedSessionState> state = std::move(state_);
    {
      const std::lock_guard<std::mutex> lock(state->mutex);
      state->socket = nullptr;
    }
    socket_->Close();
    std::unique_lock<std::mutex> lock(state->mutex);
    state->idle.wait(lock, [&state] { return state->active == 0; });
    socket_ = nullptr;
  }

 private:
  std::shared_ptr<SharedSessionState> state_;
  http::WebSocket* socket_ = nullptr;  // set iff state_ exists
};

}  // namespace internal

// The owning session handle (issue #112): what EventStream::Share() mints.
// A cheap-copy value — copies are how a session fans out, and every copy
// shares the one revocable view of the session — safe to hold beyond the
// handler borrow that produced it: a hub's registry becomes a map of these
// instead of borrowed references with manual remove-before-every-return
// discipline. The handle never extends the session: once the stream that
// minted it is gone (for a server handler, when the handler machinery
// returns), Send fails with Error::Transport — exactly what a closed stream
// returns today — and Close is a no-op, so a stale handle has no new
// failure modes, it just misses. (A moved-from handle behaves like a stale
// one.)
//
// Send and Close are const and safe from any thread; concurrent handle
// Sends serialize on the socket like concurrent stream Sends, and Send
// blocks until its frame is on the wire (fan-out wants queues on top: see
// smithy/server/session_registry.h). The one-receiver rule is unchanged —
// a handle exposes no Receive; receiving stays with the stream's owner.
template <typename Tx>
class EventStreamHandle {
 public:
  // Encodes and sends one event, blocking until its frame is on the wire
  // (the stream Send contract verbatim, encoder failures included).
  // Error::Transport once the session ended: closed, broken, or the stream
  // object destroyed.
  Outcome<Unit> Send(const Tx& event) const {
    static_assert(!std::is_same_v<Tx, NoEvents>,
                  "this stream models no events in this direction: Send is not callable on a "
                  "receive-only stream's handle (use Close)");
    auto message = encode_(event);
    if (!message.ok()) return std::move(message).error();
    http::WebSocket* socket = Acquire();
    if (socket == nullptr) return Error::Transport("event stream ended");
    auto sent = socket->Send(*message);
    Release();
    return sent;
  }

  // Initiates the close handshake; idempotent, non-blocking, safe from any
  // thread, and a no-op once the session ended. This is how a hub ends a
  // session from outside the handler (the slow-consumer policy): the
  // owner's blocked Receive/Send unblock exactly as for any other close.
  void Close() const {
    http::WebSocket* socket = Acquire();
    if (socket == nullptr) return;
    socket->Close();
    Release();
  }

  // The completion-driven send (ADR-0019): the socket's SendAsync contract
  // verbatim — one outstanding send-class operation per session, completion
  // on the transport's completion context (or inline for refusals). The
  // revocation pin spans issue to completion, so the ADR-0017 safety story
  // holds: revoking closes the session, the in-flight operation completes
  // with its error, and only then does the socket die.
  void SendAsync(const Tx& event, std::function<void(Outcome<Unit>)> callback) const {
    static_assert(!std::is_same_v<Tx, NoEvents>,
                  "this stream models no events in this direction: SendAsync is not callable on "
                  "a receive-only stream's handle (use Close)");
    auto message = encode_(event);
    if (!message.ok()) {
      callback(std::move(message).error());
      return;
    }
    http::WebSocket* socket = Acquire();
    if (socket == nullptr) {
      callback(Error::Transport("event stream ended"));
      return;
    }
    socket->SendAsync(*message,
                      [state = state_, callback = std::move(callback)](Outcome<Unit> sent) {
                        internal::ReleasePin(*state);
                        callback(std::move(sent));
                      });
  }

  // Whether the underlying session implements the async operations —
  // false for a session that already ended (a stale handle can deliver
  // nothing either way).
  bool SupportsAsync() const {
    http::WebSocket* socket = Acquire();
    if (socket == nullptr) return false;
    const bool supported = socket->SupportsAsync();
    Release();
    return supported;
  }

 private:
  template <typename T, typename R>
  friend class EventStream;
  template <typename T, typename R>
  friend class AsyncEventStream;

  EventStreamHandle(std::shared_ptr<internal::SharedSessionState> state,
                    std::function<Outcome<Message>(const Tx&)> encode)
      : state_(std::move(state)), encode_(std::move(encode)) {}

  // The socket, pinned against revocation until Release — or null once the
  // session ended (or this handle was moved from).
  http::WebSocket* Acquire() const {
    if (state_ == nullptr) return nullptr;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->socket == nullptr) return nullptr;
    ++state_->active;
    return state_->socket;
  }

  void Release() const { internal::ReleasePin(*state_); }

  std::shared_ptr<internal::SharedSessionState> state_;
  std::function<Outcome<Message>(const Tx&)> encode_;
};

// The typed event-stream session (ADR-0016): one WebSocket plus the two
// codec functions generated code supplies. Tx is what this end sends, Rx
// what it receives — a client and its server hold the same session with
// the parameters swapped. EventStream only plumbs: the encoder turns a
// typed event into a message, the decoder turns a message into the typed
// event or into the modeled Error for an exception message; the envelope
// convention lives in the codecs (smithy/eventstream/envelope.h), never
// here.
//
// Blocking, full duplex, backpressure, and cancellation are the wrapped
// WebSocket's contract verbatim: one sending and one receiving thread may
// block concurrently, and Close() from any thread is how either is
// unblocked. Share() mints handles that may outlive the stream (the hub
// seam, issue #112); the stream itself is move-only — handles are how
// ownership fans out.
template <typename Tx, typename Rx>
class EventStream {
 public:
  using Encoder = std::function<Outcome<Message>(const Tx&)>;
  using Decoder = std::function<Outcome<Rx>(const Message&)>;

  // Shares ownership of the socket — the client path
  // (BeastWebSocketClient::Dial's handle).
  EventStream(std::shared_ptr<http::WebSocket> socket, Encoder encode, Decoder decode)
      : owned_(std::move(socket)),
        socket_(owned_.get()),
        encode_(std::move(encode)),
        decode_(std::move(decode)) {}

  // Borrows the socket — the server path, where on_websocket lends a
  // WebSocket& that stays valid until the serve callback returns; the
  // stream must not outlive that borrow. (Handles from Share() may: they
  // fail softly once the stream is gone.)
  EventStream(http::WebSocket& socket, Encoder encode, Decoder decode)
      : socket_(&socket), encode_(std::move(encode)), decode_(std::move(decode)) {}

  // Destruction (and move-assignment over a live stream) ends the shared
  // view, if Share() ever minted one: view_ closes the session (unblocking
  // any handle operation mid-call), waits those operations out, and revokes
  // the socket from every handle. A stream that never called Share()
  // destructs exactly as before — nothing happens; in particular an owned
  // client stream still ends its session by Close() or by dropping the
  // socket's last reference. Copying is deleted (via the move-only view):
  // two copies would each claim that teardown; handles (Share) are how a
  // session fans out, so the stream itself stays single-owner.
  ~EventStream() = default;
  EventStream(EventStream&&) noexcept = default;
  // No explicit noexcept: the computed one depends on std::function's
  // move-assignment, which the standard leaves unspecified.
  EventStream& operator=(EventStream&&) = default;

  // Encodes and sends one event, blocking until its frame is on the wire.
  // An encoder failure surfaces as-is and leaves the session untouched;
  // wire failures are the WebSocket's (Error::Transport once closed).
  // Uncallable when Tx is NoEvents — a direction that models no events has
  // nothing to send, and the misuse is a compile error, not a runtime one.
  Outcome<Unit> Send(const Tx& event) {
    static_assert(!std::is_same_v<Tx, NoEvents>,
                  "this stream models no events in this direction: Send is not callable on a "
                  "receive-only EventStream (use Receive/Close)");
    auto message = encode_(event);
    if (!message.ok()) return std::move(message).error();
    return socket_->Send(*message);
  }

  // Blocks for the next event. nullopt is the peer's clean close — the
  // stream's natural end. A message the decoder rejects — a received
  // exception (the decoder returns it as the modeled Error) or an
  // undecodable message — is terminal (ADR-0016): the session is closed
  // and the error returned.
  Outcome<std::optional<Rx>> Receive() { return Decode(socket_->Receive()); }

  // Receive under a deadline (the socket's timed overload, typed): the
  // event, nullopt for the peer's clean close, the session's error — or
  // Error::Timeout ("TimeoutError") when `timeout` passes with nothing to
  // report. A timeout is the one failure here that spares the session: it
  // closes nothing, so the caller can assert, log, send, or wait again on
  // a stream that is still live. Decoder failures stay terminal, deadline
  // or not. Every WebSocket implements the deadline (it is pure virtual
  // there), so the bound is real whatever session this stream wraps.
  Outcome<std::optional<Rx>> Receive(std::chrono::milliseconds timeout) {
    return Decode(socket_->Receive(timeout));
  }

  // Initiates the close handshake; idempotent and safe from any thread
  // (the WebSocket contract).
  void Close() { socket_->Close(); }

  // A handle safe to hold beyond this stream's lifetime (issue #112) — the
  // hub seam: a handler passes Share() to a registry
  // (opal::server::SessionRegistry) instead of parking its borrowed
  // `stream&` in one. A cheap-copy value: all of a stream's handles (and
  // their copies) see one revocable view of the session; destroying the
  // stream closes the session and leaves them failing softly with
  // Error::Transport. Call from the thread that owns the stream (it mutates
  // the stream), as many times as you like.
  EventStreamHandle<Tx> Share() { return EventStreamHandle<Tx>(view_.Ensure(socket_), encode_); }

 private:
  // Both receive overloads: nullopt through, decode the message, and treat
  // a decoder failure as terminal (ADR-0016). A transport failure —
  // including the deadline's Error::Timeout — passes through untouched;
  // only the decoder's verdict ends the session here.
  Outcome<std::optional<Rx>> Decode(Outcome<std::optional<Message>> received) {
    if (!received.ok()) return std::move(received).error();
    std::optional<Message>& message = *received;
    if (!message.has_value()) return std::optional<Rx>();
    auto event = decode_(*message);
    if (!event.ok()) {
      Close();
      return std::move(event).error();
    }
    return std::optional<Rx>(std::move(*event));
  }

  std::shared_ptr<http::WebSocket> owned_;  // null on the borrowed path
  http::WebSocket* socket_;
  Encoder encode_;
  Decoder decode_;
  // Last member on purpose: its teardown (End) runs first and needs the
  // socket members above still alive.
  internal::SharedViewOwner view_;
};

}  // namespace opal::eventstream

#endif  // SMITHY_EVENTSTREAM_EVENT_STREAM_H_
