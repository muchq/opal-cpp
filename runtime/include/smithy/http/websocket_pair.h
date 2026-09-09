#ifndef SMITHY_HTTP_WEBSOCKET_PAIR_H_
#define SMITHY_HTTP_WEBSOCKET_PAIR_H_

#include <cstddef>
#include <memory>
#include <utility>

#include "smithy/http/websocket.h"

namespace opal::http {

// In-memory WebSocket session: two connected WebSocket ends sharing bounded
// queues, with no sockets or io threads — the Loopback analog for event
// streams (ADR-0016), so generated streaming integration tests run without
// Boost. Both ends honor the WebSocket contract:
//
// - Send blocks while the direction's queue is full (the Beast session's
//   receive-buffer bound), so backpressure is real: a receiver that stops
//   calling Receive stalls its sender. Send fails with Error::Validation
//   for a message the framing codec refuses to encode (the session stays
//   usable) and Error::Transport once the session is closed — validation
//   parity with the wire transports, minus the wire.
// - Close on EITHER end ends the session for both, mirroring the close
//   handshake; it is idempotent and unblocks blocked Send (Transport
//   error) and Receive calls on both ends. Messages queued before the
//   close still belong to their receiver, in order; Receive reports the
//   clean close (nullopt) only once its queue drains.
// - Thread safety is the WebSocket contract's: one sender plus one
//   receiver per end may block concurrently, and Close is safe from any
//   thread. There is no idle timeout — nothing here can vanish.
// - The async twins (ADR-0019) complete ready results INLINE on the
//   calling or peer thread — the pair has no executor to post to, unlike
//   the wire transports. A callback that re-arms in its own completion
//   therefore recurses; keep such volleys modest, or use the coroutine
//   adapter, whose suspend race flattens synchronous completions into
//   iteration.
class InMemoryWebSocketPair {
 public:
  // Each direction's queue bound (the Beast session's receive-buffer
  // analog): a stalled receiver blocks its sender after this many queued
  // messages. Exported so tests provoking backpressure can derive their
  // fill counts and margins from the real value instead of restating it.
  static constexpr std::size_t kQueueDepth = 8;

  // The two connected ends; each keeps the shared session alive.
  static std::pair<std::shared_ptr<WebSocket>, std::shared_ptr<WebSocket>> Create();
};

}  // namespace opal::http

#endif  // SMITHY_HTTP_WEBSOCKET_PAIR_H_
