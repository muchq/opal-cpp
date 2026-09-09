// The consumer-side integration test the quick start walks through: implement
// the generated handler, then drive the generated server with the generated
// client over the loopback transport and a real socket.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "acme/todo/cbor/client.h"
#include "acme/todo/cbor/server.h"
#include "acme/todo/client.h"
#include "acme/todo/jsonrpc/client.h"
#include "acme/todo/jsonrpc/server.h"
#include "acme/todo/server.h"
#include "opal/client/config.h"
#include "opal/core/error.h"
#include "opal/http/forwarded.h"
#include "opal/http/loopback.h"
#include "opal/http/socket_transport.h"
#include "opal/http/trace_context.h"
#include "opal/server/middleware.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::NoSuchTask;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;

// [quickstart:handler] This exact block is the handler docs/quickstart.md
// teaches; QuickstartMirrorTest fails if the two ever diverge.
class InMemoryHandler final : public TodoHandler {
 public:
  opal::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                       const opal::server::RequestContext&) override {
    const std::lock_guard<std::mutex> lock(mu_);
    const std::string id = "task-" + std::to_string(next_id_++);
    titles_[id] = input.title;
    return AddTaskOutput{.taskId = id, .title = input.title};
  }

  opal::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                       const opal::server::RequestContext&) override {
    const std::lock_guard<std::mutex> lock(mu_);
    const auto it = titles_.find(input.taskId);
    if (it == titles_.end()) {
      opal::Error error = opal::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
      return error;  // the server turns this into the modeled 404
    }
    return GetTaskOutput{.taskId = input.taskId, .title = it->second, .done = false};
  }

 private:
  std::mutex mu_;  // handlers must be thread-safe: transports dispatch on a thread pool
  int next_id_ = 1;
  std::map<std::string, std::string> titles_;
};
// [/quickstart:handler]

enum class Transport { kLoopback, kSocket };

class TodoIntegrationTest : public ::testing::TestWithParam<Transport> {
 protected:
  void SetUp() override {
    server_ = std::make_unique<TodoServer>(std::make_shared<InMemoryHandler>());
    opal::ClientConfig config;
    if (GetParam() == Transport::kLoopback) {
      auto loopback = std::make_shared<opal::http::Loopback>();
      ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
      config.http_client = loopback;
    } else {
      socket_server_ = std::make_unique<opal::http::SocketHttpServer>();
      ASSERT_TRUE(socket_server_->Start(server_->Handler()).ok());
      config.endpoint = "http://127.0.0.1:" + std::to_string(socket_server_->port());
    }
    auto client = TodoClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<TodoClient>(std::move(*client));
  }

  void TearDown() override {
    if (socket_server_ != nullptr) socket_server_->Stop();
  }

  std::unique_ptr<TodoServer> server_;
  std::unique_ptr<opal::http::SocketHttpServer> socket_server_;
  std::unique_ptr<TodoClient> client_;
};

TEST_P(TodoIntegrationTest, AddThenGetRoundTrips) {
  const auto added = client_->AddTask(AddTaskInput{.title = "write the quick start"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "write the quick start");

  const auto fetched = client_->GetTask(GetTaskInput{.taskId = added->taskId});
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();
  EXPECT_EQ(fetched->title, "write the quick start");
}

TEST_P(TodoIntegrationTest, ModeledErrorsSurfaceTyped) {
  const auto missing = client_->GetTask(GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
  ASSERT_NE(missing.error().detail<NoSuchTask>(), nullptr);
  EXPECT_EQ(missing.error().detail<NoSuchTask>()->message, "no task: nope");
}

TEST_P(TodoIntegrationTest, ConstraintValidationRejectsBeforeTheHandler) {
  // @length(min: 1) on title: the framework answers 400 ValidationException.
  const auto rejected = client_->AddTask(AddTaskInput{.title = ""});
  ASSERT_FALSE(rejected.ok());
}

INSTANTIATE_TEST_SUITE_P(Transports, TodoIntegrationTest,
                         ::testing::Values(Transport::kLoopback, Transport::kSocket),
                         [](const auto& info) {
                           return info.param == Transport::kLoopback ? "Loopback" : "Socket";
                         });

// The same protocol-agnostic model, bound to rpcv2Cbor by a different
// `apply` overlay (model/bindings/rpcv2cbor.smithy): identical handler
// semantics over a completely different wire protocol.
TEST(TodoCborTest, SameModelServesRpcv2Cbor) {
  class CborHandler final : public acme::todo::cbor::TodoHandler {
   public:
    opal::Outcome<acme::todo::cbor::AddTaskOutput> AddTask(
        const acme::todo::cbor::AddTaskInput& input, const opal::server::RequestContext&) override {
      return acme::todo::cbor::AddTaskOutput{.taskId = "task-1", .title = input.title};
    }
    opal::Outcome<acme::todo::cbor::GetTaskOutput> GetTask(
        const acme::todo::cbor::GetTaskInput& input, const opal::server::RequestContext&) override {
      opal::Error error = opal::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(acme::todo::cbor::NoSuchTask{.message = "no task: " + input.taskId});
      return error;
    }
  };

  acme::todo::cbor::TodoServer server(std::make_shared<CborHandler>());
  auto loopback = std::make_shared<opal::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());
  opal::ClientConfig config;
  config.http_client = loopback;
  auto client = acme::todo::cbor::TodoClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto added = client->AddTask(acme::todo::cbor::AddTaskInput{.title = "ship it"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->taskId, "task-1");
  EXPECT_EQ(added->title, "ship it");

  const auto missing = client->GetTask(acme::todo::cbor::GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
}

// And a third overlay (model/bindings/jsonrpc2.smithy) binds the same model
// to JSON-RPC 2.0: a single POST / endpoint dispatching on the envelope's
// method member.
TEST(TodoJsonRpcTest, SameModelServesJsonRpc2) {
  class JsonRpcHandler final : public acme::todo::jsonrpc::TodoHandler {
   public:
    opal::Outcome<acme::todo::jsonrpc::AddTaskOutput> AddTask(
        const acme::todo::jsonrpc::AddTaskInput& input,
        const opal::server::RequestContext&) override {
      return acme::todo::jsonrpc::AddTaskOutput{.taskId = "task-1", .title = input.title};
    }
    opal::Outcome<acme::todo::jsonrpc::GetTaskOutput> GetTask(
        const acme::todo::jsonrpc::GetTaskInput& input,
        const opal::server::RequestContext&) override {
      opal::Error error = opal::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
      error.set_detail(acme::todo::jsonrpc::NoSuchTask{.message = "no task: " + input.taskId});
      return error;
    }
  };

  acme::todo::jsonrpc::TodoServer server(std::make_shared<JsonRpcHandler>());
  auto loopback = std::make_shared<opal::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());
  opal::ClientConfig config;
  config.http_client = loopback;
  auto client = acme::todo::jsonrpc::TodoClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto added = client->AddTask(acme::todo::jsonrpc::AddTaskInput{.title = "ship it"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->taskId, "task-1");
  EXPECT_EQ(added->title, "ship it");

  const auto missing = client->GetTask(acme::todo::jsonrpc::GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
  ASSERT_NE(missing.error().detail<acme::todo::jsonrpc::NoSuchTask>(), nullptr);
}

// Framework-level routing at the module boundary: requests no generated
// client would send (wrong method, unknown path) get the router's shaped
// answers — 405 with a deterministic, deduplicated Allow list, and 404.
TEST(TodoRoutingTest, WrongMethodGets405WithAllowAndUnknownPathGets404) {
  TodoServer server(std::make_shared<InMemoryHandler>());
  auto loopback = std::make_shared<opal::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());

  opal::http::HttpRequest wrong_method;
  wrong_method.method = "DELETE";
  wrong_method.target = "/tasks";
  const auto not_allowed = loopback->Send(wrong_method);
  ASSERT_TRUE(not_allowed.ok());
  EXPECT_EQ(not_allowed->status, 405);
  EXPECT_EQ(not_allowed->headers.Get("allow").value_or(""), "POST");

  opal::http::HttpRequest unknown;
  unknown.method = "GET";
  unknown.target = "/no/such/route";
  const auto not_found = loopback->Send(unknown);
  ASSERT_TRUE(not_found.ok());
  EXPECT_EQ(not_found->status, 404);
}

// The handler-visible request context (ADR-0010) at the module boundary:
// the raw request — unmodeled headers, the transport-stamped peer address,
// the inbound traceparent — reaches a consumer's handler.
TEST(TodoMetadataTest, RestHandlerSeesHeadersPeerAndTraceOverARealSocket) {
  class MetadataHandler final : public TodoHandler {
   public:
    opal::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                         const opal::server::RequestContext& context) override {
      const auto trace =
          opal::http::ParseTraceparent(context.request->headers.Get("traceparent").value_or(""));
      return AddTaskOutput{.taskId = context.request->peer_address,
                           .title = input.title + "|" +
                                    context.request->headers.Get("x-tenant").value_or("missing") +
                                    "|" + (trace.has_value() ? trace->trace_id : "no-trace")};
    }
    opal::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                         const opal::server::RequestContext&) override {
      return opal::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    }
  };

  TodoServer server(std::make_shared<MetadataHandler>());
  opal::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  // A raw request so unmodeled headers ride along (generated clients only
  // send what the model binds).
  opal::http::SocketHttpClient raw("127.0.0.1", transport.port());
  opal::http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  request.headers.Set("content-type", "application/json");
  request.headers.Set("x-tenant", "acme");
  request.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  request.body = R"({"title":"trace me"})";
  const auto response = raw.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_NE(response->body.find("trace me|acme|0af7651916cd43dd8448eb211c80319c"),
            std::string::npos)
      << response->body;
  EXPECT_NE(response->body.find("127.0.0.1:"), std::string::npos) << response->body;

  // And with no inbound traceparent at all, the ingress mints one
  // (ADR-0011): the handler still sees a parseable identity, never
  // "no-trace".
  opal::http::HttpRequest bare;
  bare.method = "POST";
  bare.target = "/tasks";
  bare.headers.Set("content-type", "application/json");
  bare.body = R"({"title":"minted"})";
  const auto minted = raw.Send(bare);
  ASSERT_TRUE(minted.ok()) << minted.error().message();
  EXPECT_EQ(minted->body.find("no-trace"), std::string::npos) << minted->body;
  EXPECT_NE(minted->body.find("minted|missing|"), std::string::npos) << minted->body;
  transport.Stop();
}

// A returned (not thrown) unmodeled error correlates too: the generated
// server maps it to a 500 InternalFailure, and the ingress identity arrives
// as x-correlation-id (ADR-0011). Modeled errors are expected outcomes and
// stay unstamped.
TEST(TodoMetadataTest, ReturnedServerErrorsCarryTheTraceCorrelationId) {
  class FailingHandler final : public TodoHandler {
   public:
    opal::Outcome<AddTaskOutput> AddTask(const AddTaskInput&,
                                         const opal::server::RequestContext&) override {
      return opal::Error::Transport("db down");
    }
    opal::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                         const opal::server::RequestContext&) override {
      return opal::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    }
  };

  TodoServer server(std::make_shared<FailingHandler>());
  opal::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());
  opal::http::SocketHttpClient raw("127.0.0.1", transport.port());

  opal::http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  request.headers.Set("content-type", "application/json");
  request.headers.Set("traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
  request.body = R"({"title":"boom"})";
  const auto failed = raw.Send(request);
  ASSERT_TRUE(failed.ok()) << failed.error().message();
  EXPECT_EQ(failed->status, 500);
  EXPECT_EQ(failed->headers.Get("x-correlation-id").value_or(""),
            "0af7651916cd43dd8448eb211c80319c");

  opal::http::HttpRequest missing;
  missing.method = "GET";
  missing.target = "/tasks/nope";
  const auto modeled = raw.Send(missing);
  ASSERT_TRUE(modeled.ok()) << modeled.error().message();
  EXPECT_EQ(modeled->status, 404);
  EXPECT_FALSE(modeled->headers.Get("x-correlation-id").has_value());
  transport.Stop();
}

// The production middleware chain from the guide — Guard, then Observe, then
// liveness and readiness HealthEndpoints — composed around a generated server
// and driven by the generated client.
TEST(TodoMiddlewareTest, GuardObserveAndHealthComposeAroundTheServer) {
  TodoServer server(std::make_shared<InMemoryHandler>());
  int started = 0;
  int completed = 0;
  bool admit = true;
  bool ready = true;
  auto handler = opal::server::Chain(
      {opal::server::Guard([&admit](const opal::http::HttpRequest&) { return admit; },
                           opal::server::TooManyRequests(std::chrono::seconds(1))),
       // Observe takes on_complete first, then the optional on_start.
       opal::server::Observe([&completed](const opal::server::RequestObservation&) { ++completed; },
                             [&started](const opal::server::RequestStart&) { ++started; }),
       opal::server::HealthEndpoint(),
       opal::server::HealthEndpoint("/readyz", {{"db", [&ready] { return ready; }}})},
      server.Handler());

  auto loopback = std::make_shared<opal::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  // Liveness answers without reaching the router, and is observed.
  opal::http::HttpRequest health;
  health.method = "GET";
  health.target = "/health";
  const auto health_response = loopback->Send(health);
  ASSERT_TRUE(health_response.ok());
  EXPECT_EQ(health_response->status, 200);
  EXPECT_EQ(health_response->body, R"({"status":"healthy"})");

  // Readiness re-probes on every request: 200 while the dependency serves,
  // 503 naming it once it stops.
  opal::http::HttpRequest readyz;
  readyz.method = "GET";
  readyz.target = "/readyz";
  const auto ready_response = loopback->Send(readyz);
  ASSERT_TRUE(ready_response.ok());
  EXPECT_EQ(ready_response->status, 200);
  ready = false;
  const auto unready_response = loopback->Send(readyz);
  ASSERT_TRUE(unready_response.ok());
  EXPECT_EQ(unready_response->status, 503);
  EXPECT_EQ(unready_response->body, R"({"status":"unhealthy","failing":["db"]})");

  // The generated client works through the chain.
  opal::ClientConfig config;
  config.http_client = loopback;
  auto created = TodoClient::Create(std::move(config));
  ASSERT_TRUE(created.ok()) << created.error().message();
  TodoClient client = std::move(*created);
  const auto added = client.AddTask(AddTaskInput{.title = "compose middleware"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "compose middleware");

  // Once admit flips, Guard sheds load with the shaped 429 before Observe.
  admit = false;
  opal::http::HttpRequest denied;
  denied.method = "POST";
  denied.target = "/tasks";
  const auto denied_response = loopback->Send(denied);
  ASSERT_TRUE(denied_response.ok());
  EXPECT_EQ(denied_response->status, 429);
  EXPECT_EQ(denied_response->headers.Get("retry-after").value_or(""), "1");

  // health + two readyz + AddTask; the rejected request never reached Observe.
  EXPECT_EQ(started, 4);
  EXPECT_EQ(completed, 4);
}

// ADR-0012 composed at the consumer boundary, over a real socket so the
// transport stamps a real peer — through the shipped PerClientRateLimit
// (issue #104), the exact middleware the production guide teaches:
// x-forwarded-for drives policy only through a trusted proxy tier and is
// client-authored noise otherwise. The allow policy records the derived
// key — the boundary claim is that a real transport-stamped peer flows
// through the derivation, which the status alone cannot prove.
TEST(TodoMiddlewareTest, PerClientRateLimitKeysOnTheDerivedClientAddressNotTheSpoofableHeader) {
  std::string seen;
  const auto deny_banned = [&seen](const opal::http::TrustedProxies& trusted) {
    return opal::server::PerClientRateLimit(
        [&seen](const std::string& client) {
          seen = client;
          return client != "203.0.113.9";
        },
        trusted);
  };
  TodoServer server(std::make_shared<InMemoryHandler>());

  opal::http::HttpRequest add;
  add.method = "POST";
  add.target = "/tasks";
  add.headers.Set("content-type", "application/json");
  add.body = R"({"title":"who goes there"})";

  {
    // Trusting loopback as the proxy tier: the headerless request is the
    // trusted-tier path and keys as the stamped peer itself; the banned
    // client is seen through the appended entry — the walk never reaches
    // the spoofed prefix — and shed as the shaped 429.
    opal::http::SocketHttpServer transport;
    ASSERT_TRUE(transport
                    .Start(opal::server::Chain(
                        {deny_banned(*opal::http::TrustedProxies::Parse({"127.0.0.0/8"}))},
                        server.Handler()))
                    .ok());
    opal::http::SocketHttpClient raw("127.0.0.1", transport.port());

    const auto direct = raw.Send(add);
    ASSERT_TRUE(direct.ok()) << direct.error().message();
    EXPECT_EQ(direct->status, 200);
    EXPECT_EQ(seen, "127.0.0.1");

    opal::http::HttpRequest banned = add;
    banned.headers.Set("x-forwarded-for", "198.51.100.7, 203.0.113.9");
    const auto denied = raw.Send(banned);
    ASSERT_TRUE(denied.ok()) << denied.error().message();
    EXPECT_EQ(denied->status, 429);
    EXPECT_EQ(seen, "203.0.113.9");
    transport.Stop();
  }
  {
    // The direct-connect deployment (no trust configured): the same header
    // is ignored wholly, the peer stays the key, the request is admitted.
    opal::http::SocketHttpServer transport;
    ASSERT_TRUE(transport
                    .Start(opal::server::Chain({deny_banned(opal::http::TrustedProxies::None())},
                                               server.Handler()))
                    .ok());
    opal::http::SocketHttpClient raw("127.0.0.1", transport.port());

    opal::http::HttpRequest spoofed = add;
    spoofed.headers.Set("x-forwarded-for", "203.0.113.9");
    const auto admitted = raw.Send(spoofed);
    ASSERT_TRUE(admitted.ok()) << admitted.error().message();
    EXPECT_EQ(admitted->status, 200);
    EXPECT_EQ(seen, "127.0.0.1");
    transport.Stop();
  }
}

// Out-of-tree acceptance for the request-line injection defense (issue
// #109): a consumer hand-building an HttpRequest with a CR/LF-bearing
// target or method must have the client refuse before any bytes reach the
// wire — the request-line analog of the header-injection guard exercised
// through the module boundary, with a live server proving nothing is sent.
TEST(TodoRequestLineInjectionTest, ARawClientRefusesCrlfInTargetOrMethodAndServesCleanOnes) {
  TodoServer server(std::make_shared<InMemoryHandler>());
  opal::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());
  opal::http::SocketHttpClient client("127.0.0.1", transport.port());

  // A smuggled second request line hidden in the target: refused, not sent.
  opal::http::HttpRequest injected_target;
  injected_target.method = "POST";
  injected_target.target = "/tasks HTTP/1.1\r\nX-Smuggled: 1\r\n\r\nGET /tasks";
  injected_target.headers.Set("content-type", "application/json");
  injected_target.body = R"({"title":"x"})";
  const auto target_outcome = client.Send(injected_target);
  ASSERT_FALSE(target_outcome.ok());
  EXPECT_EQ(target_outcome.error().kind(), opal::ErrorKind::kValidation);

  // Same in the method.
  opal::http::HttpRequest injected_method;
  injected_method.method = "POST /evil HTTP/1.1\r\nX-Smuggled: 1\r\n\r\nGET";
  injected_method.target = "/tasks";
  const auto method_outcome = client.Send(injected_method);
  ASSERT_FALSE(method_outcome.ok());
  EXPECT_EQ(method_outcome.error().kind(), opal::ErrorKind::kValidation);

  // A legitimate request on the same client still works — the guard is not
  // a false positive.
  opal::http::HttpRequest clean;
  clean.method = "POST";
  clean.target = "/tasks";
  clean.headers.Set("content-type", "application/json");
  clean.body = R"({"title":"clean"})";
  const auto clean_outcome = client.Send(clean);
  ASSERT_TRUE(clean_outcome.ok()) << clean_outcome.error().message();
  EXPECT_EQ(clean_outcome->status, 200);

  transport.Stop();
}

}  // namespace
