// The multi-file model check: greeter.smithy (protocol-agnostic) plus the
// greeter_restjson1.smithy `apply` overlay assemble into one model inside the
// build graph, and the generated client and server actually work together.

#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "opal/client/config.h"
#include "opal/http/loopback.h"
#include "smithy/cpp/ruletest/client.h"
#include "smithy/cpp/ruletest/server.h"

namespace smithy::cpp::ruletest {
namespace {

class Handler final : public GreeterHandler {
 public:
  opal::Outcome<GreetOutput> Greet(const GreetInput& input,
                                   const opal::server::RequestContext&) override {
    return GreetOutput{.greeting = "hello, " + input.name};
  }
};

TEST(GreeterMultiModelTest, OverlayBoundServiceRoundTrips) {
  GreeterServer server(std::make_shared<Handler>());
  auto loopback = std::make_shared<opal::http::Loopback>();
  ASSERT_TRUE(loopback->Start(server.Handler()).ok());

  opal::ClientConfig config;
  config.http_client = loopback;
  auto client = GreeterClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto greeting = client->Greet(GreetInput{.name = "smithy"});
  ASSERT_TRUE(greeting.ok()) << greeting.error().message();
  EXPECT_EQ(greeting->greeting, "hello, smithy");
}

}  // namespace
}  // namespace smithy::cpp::ruletest
