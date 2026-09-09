// Out-of-tree acceptance for the runtime's exception-containment contract
// (ADR-0003, issue #109 item C): from a separate Bazel module, an
// application callback that throws — a request handler, a WebSocket
// completion — must never unwind a transport io thread and terminate the
// process. The server contains it and keeps serving; the stream contains it
// and keeps working. A regression here would crash the whole test binary
// rather than fail an assertion, which is exactly the failure mode the
// containment exists to prevent.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>

#include "opal/client/config.h"
#include "opal/eventstream/frame.h"
#include "opal/http/beast_transport.h"
#include "opal/http/websocket.h"

namespace {

using opal::eventstream::Message;
using opal::http::BeastHttpClient;
using opal::http::BeastServerTransport;
using opal::http::BeastWebSocketClient;
using opal::http::HttpRequest;
using opal::http::HttpResponse;
using opal::http::WebSocket;

HttpRequest Get(const std::string& target) {
  HttpRequest request;
  request.method = "GET";
  request.target = target;
  return request;
}

Message Event(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = opal::Blob::FromString(body)};
}

TEST(ExceptionSafetyAcceptanceTest, AThrowingHandlerBecomesA500AndTheServerKeepsServing) {
  // The handler throws on one route and answers on another. The throw must
  // surface to the client as a contained 500 — not a dropped connection or a
  // dead server — and the very next request on a healthy route must succeed,
  // proving the io threads survived.
  BeastServerTransport server(BeastServerTransport::Options{.threads = 2, .handler_threads = 4});
  const bool started = server
                           .Start([](const HttpRequest& request) -> HttpResponse {
                             if (request.target == "/throw") {
                               throw std::runtime_error("handler blew up across the boundary");
                             }
                             HttpResponse response;
                             response.status = 200;
                             response.body = "ok";
                             return response;
                           })
                           .ok();
  ASSERT_TRUE(started);

  opal::ClientConfig config;
  config.endpoint = "http://127.0.0.1:" + std::to_string(server.port());
  auto client = BeastHttpClient::FromConfig(config);
  ASSERT_TRUE(client.ok()) << client.error().message();

  // The throwing route is contained as a correlated 500.
  auto thrown = (*client)->Send(Get("/throw"));
  ASSERT_TRUE(thrown.ok()) << thrown.error().message();
  EXPECT_EQ(thrown->status, 500);

  // The server is unharmed: a healthy route still answers over the same
  // transport, hit a few times to land on more than one io/handler thread.
  for (int i = 0; i < 5; ++i) {
    auto ok = (*client)->Send(Get("/ok"));
    ASSERT_TRUE(ok.ok()) << ok.error().message();
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(ok->body, "ok");
  }

  server.Stop();
}

TEST(ExceptionSafetyAcceptanceTest, AThrowingAsyncReceiveCallbackSparesTheStream) {
  // A WebSocket completion callback that throws runs on the connection's io
  // thread. Contained there, it neither terminates the process nor abandons
  // the read pump, so the stream round-trips again afterward.
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) return;
      if (!socket.Send(Event("pong", "echo:" + (*message)->payload.ToString())).ok()) return;
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 404;
                    return response;
                  })
                  .ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const auto& socket = *dialed;

  std::promise<void> fired;
  socket->ReceiveAsync([&fired](opal::Outcome<std::optional<Message>> message) {
    ASSERT_TRUE(message.ok()) << message.error().message();
    fired.set_value();
    throw std::runtime_error("consumer receive callback blew up");
  });
  ASSERT_TRUE(socket->Send(Event("ping", "first")).ok());
  ASSERT_EQ(fired.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

  // The session survived the throw: another message round-trips.
  ASSERT_TRUE(socket->Send(Event("ping", "second")).ok());
  auto echo = socket->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:second");

  socket->Close();
  server.Stop();
}

}  // namespace
