#ifndef OPAL_EXAMPLES_JSONRPC2_STREAM_TEST_FIXTURE_H_
#define OPAL_EXAMPLES_JSONRPC2_STREAM_TEST_FIXTURE_H_

// The directory's in-memory e2e fixture, the chat stream_test_fixture
// transposed to the jsonRpc2 wire (ADR-0023): a generated CalculatorClient
// whose websocket_dialer hands back one end of an InMemoryWebSocketPair and
// serves the other end through the generated StreamRouter, synthesizing the
// shared-endpoint upgrade a real transport would deliver, with a Loopback
// carrying the unary neighbors. Because the JSON-RPC translation is a
// wrapper above the socket, the pair carries the ACTUAL envelope text —
// wire-level tests speak raw headerless messages on an unserved far end.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example/calculator/client.h"
#include "example/calculator/server.h"
#include "opal/client/config.h"
#include "opal/core/outcome.h"
#include "opal/http/loopback.h"
#include "opal/http/message.h"
#include "opal/http/websocket.h"
#include "opal/http/websocket_pair.h"

namespace example::calculator {

class StreamTestFixture : public testing::Test {
 protected:
  void StartWith(std::shared_ptr<CalculatorHandler> handler) {
    Start(std::make_unique<CalculatorServer>(std::move(handler)), /*session_seam=*/false);
  }

  // The ADR-0021 wiring: the same server around the ASYNC handler, its
  // stream route dispatched through ServeSession. The launch point runs
  // inline in the dialer and returns immediately; the pair's inline
  // completions then drive the coroutines, so no serve thread exists.
  void StartWithAsync(std::shared_ptr<CalculatorAsyncHandler> handler) {
    Start(std::make_unique<CalculatorServer>(std::move(handler)), /*session_seam=*/true);
  }

  void Start(std::unique_ptr<CalculatorServer> server, bool session_seam) {
    server_ = std::move(server);
    session_seam_ = session_seam;
    auto loopback = std::make_shared<opal::http::Loopback>();
    ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = loopback;  // the unary neighbors' transport
    config.websocket_dialer = [this](const opal::http::WebSocketDialRequest& request)
        -> opal::Outcome<std::shared_ptr<opal::http::WebSocket>> {
      last_dialed_target_ = request.target;
      auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
      sessions_.push_back(far);
      Serve(request.target, far);
      return near;
    };
    auto client = CalculatorClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<CalculatorClient>(std::move(*client));
  }

  // Routes one server end through the generated StreamRouter, the way the
  // transport would after the upgrade. Also used directly by wire-level
  // tests that speak raw envelopes on the near end instead of dialing.
  void Serve(const std::string& target, std::shared_ptr<opal::http::WebSocket> session) {
    opal::http::HttpRequest upgrade;
    upgrade.method = "GET";
    upgrade.target = target;
    if (session_seam_) {
      server_->StreamRouter()->ServeSession()(upgrade, std::move(session));  // a launch point
    } else {
      threads_.emplace_back([serve = server_->StreamRouter()->Serve(), upgrade,
                             session = std::move(session)] { serve(upgrade, *session); });
    }
  }

  void TearDown() override {
    // Close is idempotent; this unblocks any serve loop a failed test body
    // left mid-Receive, so the joins below cannot hang.
    for (auto& session : sessions_) session->Close();
    for (std::thread& thread : threads_) thread.join();
  }

  std::unique_ptr<CalculatorServer> server_;
  std::unique_ptr<CalculatorClient> client_;
  // Every dialed or hand-made far end — closed in TearDown regardless.
  std::vector<std::shared_ptr<opal::http::WebSocket>> sessions_;
  std::vector<std::thread> threads_;
  std::string last_dialed_target_;
  bool session_seam_ = false;
};

}  // namespace example::calculator

#endif  // OPAL_EXAMPLES_JSONRPC2_STREAM_TEST_FIXTURE_H_
