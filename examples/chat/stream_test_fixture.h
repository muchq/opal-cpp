#ifndef OPAL_EXAMPLES_CHAT_STREAM_TEST_FIXTURE_H_
#define OPAL_EXAMPLES_CHAT_STREAM_TEST_FIXTURE_H_

// The directory's one in-memory e2e fixture — the server guide's documented
// recipe (InMemoryWebSocketPair + an injected dialer), kept in a single
// place for the suites that share it (chat_e2e_test.cc, hub_e2e_test.cc):
// a generated ChatClient whose websocket_dialer hands back one end of a
// pair and serves the other end through the generated StreamRouter on a
// thread, synthesizing the upgrade request a real transport would deliver,
// with a Loopback carrying the unary neighbor. TearDown closes every far
// end first, so a serve loop a failed test body left mid-Receive cannot
// hang the joins.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example/chat/client.h"
#include "example/chat/server.h"
#include "opal/client/config.h"
#include "opal/core/outcome.h"
#include "opal/http/loopback.h"
#include "opal/http/message.h"
#include "opal/http/websocket.h"
#include "opal/http/websocket_pair.h"

namespace example::chat {

class StreamTestFixture : public testing::Test {
 protected:
  // Builds the generated server around `handler` and wires both client
  // seams (unary Loopback, streaming dialer). Callable again mid-test to
  // restart with a different handler; earlier sessions stay owned until
  // TearDown.
  void StartWith(std::shared_ptr<ChatHandler> handler) {
    Start(std::make_unique<ChatServer>(std::move(handler)), /*session_seam=*/false);
  }

  // The ADR-0021 wiring: the same server around the ASYNC handler, its
  // streaming routes dispatched through ServeSession. The launch point
  // runs inline in the dialer and returns immediately; the pair's inline
  // completions then drive the coroutine, so no serve thread exists.
  void StartWithAsync(std::shared_ptr<ChatAsyncHandler> handler) {
    Start(std::make_unique<ChatServer>(std::move(handler)), /*session_seam=*/true);
  }

  void Start(std::unique_ptr<ChatServer> server, bool session_seam) {
    server_ = std::move(server);
    auto loopback = std::make_shared<opal::http::Loopback>();
    ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = loopback;  // the unary neighbor's transport
    config.websocket_dialer = [this, session_seam](const opal::http::WebSocketDialRequest& request)
        -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
      last_dialed_target_ = request.target;
      auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
      opal::http::HttpRequest upgrade;
      upgrade.method = "GET";
      upgrade.target = request.target;
      upgrade.headers = request.headers;
      sessions_.push_back(far);
      if (serve_far_end_) {
        if (session_seam) {
          server_->StreamRouter()->ServeSession()(upgrade, far);  // a launch point, not a loop
        } else {
          threads_.emplace_back([serve = server_->StreamRouter()->Serve(), upgrade, session = far] {
            serve(upgrade, *session);
          });
        }
      }
      return near;
    };
    auto client = ChatClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<ChatClient>(std::move(*client));
  }

  void TearDown() override {
    // Close is idempotent; this unblocks any serve loop a failed test body
    // left mid-Receive, so the joins below cannot hang.
    for (auto& session : sessions_) session->Close();
    for (std::thread& thread : threads_) thread.join();
  }

  std::unique_ptr<ChatServer> server_;
  std::unique_ptr<ChatClient> client_;
  // Every dialed far end — closed in TearDown even when left unserved.
  std::vector<std::shared_ptr<opal::http::WebSocket>> sessions_;
  std::vector<std::thread> threads_;
  std::string last_dialed_target_;
  // Cleared by a test that wants the dialed pair's far end raw — held in
  // sessions_ (so TearDown still closes it) but not served through the
  // router, leaving the test free to speak the wire itself.
  bool serve_far_end_ = true;
};

}  // namespace example::chat

#endif  // OPAL_EXAMPLES_CHAT_STREAM_TEST_FIXTURE_H_
