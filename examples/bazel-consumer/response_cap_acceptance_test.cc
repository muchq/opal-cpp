// Out-of-tree acceptance for ClientConfig::max_response_bytes (issue #189):
// a generated client, wired the way the quickstart wires it (endpoint only,
// so Create() builds the transport), refuses a response larger than the
// budget the config gives it, and says which knob to raise. The failure is
// not retryable — a server that sent 4 KiB will send 4 KiB again — so the
// retry loop does not spend attempts on it.
//
// The resource is a blob @httpPayload (the redirector model's Fetch), the
// response shape this cap exists for: a download whose size the model does
// not bound.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "acme/redirect/client.h"
#include "acme/redirect/server.h"
#include "opal/client/config.h"
#include "opal/core/error.h"
#include "opal/http/socket_transport.h"

namespace {

using acme::redirect::DownloadDynamicInput;
using acme::redirect::DownloadDynamicOutput;
using acme::redirect::DownloadInput;
using acme::redirect::DownloadOutput;
using acme::redirect::FetchInput;
using acme::redirect::FetchOutput;
using acme::redirect::NoSuchSlug;
using acme::redirect::ProbeInput;
using acme::redirect::ProbeOutput;
using acme::redirect::RedirectorClient;
using acme::redirect::RedirectorHandler;
using acme::redirect::RedirectorServer;
using acme::redirect::ResolveDynamicInput;
using acme::redirect::ResolveDynamicOutput;
using acme::redirect::ResolveInput;
using acme::redirect::ResolveOutput;

constexpr std::size_t kPayloadBytes = 4096;

// Serves a fixed 4 KiB payload under every slug.
class LargePayloadHandler final : public RedirectorHandler {
 public:
  opal::Outcome<FetchOutput> Fetch(const FetchInput&,
                                   const opal::server::RequestContext&) override {
    return FetchOutput{.status = 200,
                       .etag = "\"big\"",
                       .content = opal::Blob::FromString(std::string(kPayloadBytes, 'p'))};
  }
  opal::Outcome<ProbeOutput> Probe(const ProbeInput&,
                                   const opal::server::RequestContext&) override {
    return ProbeOutput{.etag = "\"big\"",
                       .content = opal::Blob::FromString(std::string(kPayloadBytes, 'p'))};
  }
  opal::Outcome<DownloadDynamicOutput> DownloadDynamic(
      const DownloadDynamicInput& input, const opal::server::RequestContext&) override {
    (void)input;
    return DownloadDynamicOutput{
        .status = 200,
        .etag = "\"big\"",
        .content = opal::Blob::FromString(std::string(kPayloadBytes, 'p'))};
  }

  // The streaming sibling of Fetch (#213). Not this file's subject; it
  // answers the same resource so the files cannot drift on what is served.
  opal::Outcome<DownloadOutput> Download(const DownloadInput& input,
                                         const opal::server::RequestContext&) override {
    (void)input;
    return DownloadOutput{.etag = "\"big\"",
                          .content = opal::Blob::FromString(std::string(kPayloadBytes, 'p'))};
  }

  opal::Outcome<ResolveOutput> Resolve(const ResolveInput& input,
                                       const opal::server::RequestContext&) override {
    return NotFound(input.slug);
  }
  opal::Outcome<ResolveDynamicOutput> ResolveDynamic(const ResolveDynamicInput& input,
                                                     const opal::server::RequestContext&) override {
    return NotFound(input.slug);
  }

 private:
  static opal::Error NotFound(const std::string& slug) {
    opal::Error error = opal::Error::Modeled("NoSuchSlug", "no slug: " + slug);
    error.set_detail(NoSuchSlug{.message = "no slug: " + slug});
    return error;
  }
};

class ResponseCapAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(transport_.Start(server_.Handler()).ok());
    config_.endpoint = "http://127.0.0.1:" + std::to_string(transport_.port());
    config_.retry.max_attempts = 3;
    config_.retry.sleep = [this](std::chrono::milliseconds) { ++sleeps_; };
  }
  void TearDown() override { transport_.Stop(); }

  RedirectorServer server_{std::make_shared<LargePayloadHandler>()};
  opal::http::SocketHttpServer transport_;
  opal::ClientConfig config_;
  int sleeps_ = 0;
};

TEST_F(ResponseCapAcceptanceTest, AResponseOverTheBudgetFailsNamingTheKnobAndIsNotRetried) {
  config_.max_response_bytes = kPayloadBytes - 1;
  auto client = RedirectorClient::Create(config_);
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto fetched = client->Fetch(FetchInput{.slug = "abc"});
  ASSERT_FALSE(fetched.ok());
  EXPECT_EQ(fetched.error().kind(), opal::ErrorKind::kTransport);
  EXPECT_FALSE(fetched.error().retryable());
  EXPECT_NE(fetched.error().message().find("max_response_bytes"), std::string::npos)
      << fetched.error().message();
  EXPECT_EQ(sleeps_, 0) << "the retry loop backed off for a response that cannot shrink";
}

TEST_F(ResponseCapAcceptanceTest, AResponseAtTheBudgetIsDeliveredWhole) {
  config_.max_response_bytes = kPayloadBytes;
  auto client = RedirectorClient::Create(config_);
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto fetched = client->Fetch(FetchInput{.slug = "abc"});
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();
  ASSERT_TRUE(fetched->content.has_value());
  EXPECT_EQ(fetched->content->size(), kPayloadBytes);
}

TEST_F(ResponseCapAcceptanceTest, TheDefaultBudgetIsGenerousAndNamed) {
  // The default is the same 64 MiB the server side caps requests at; a
  // consumer that never sets the knob gets that, not "unlimited".
  EXPECT_EQ(opal::ClientConfig{}.max_response_bytes, std::size_t{64} * 1024 * 1024);
  auto client = RedirectorClient::Create(config_);
  ASSERT_TRUE(client.ok()) << client.error().message();
  const auto fetched = client->Fetch(FetchInput{.slug = "abc"});
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();
  ASSERT_TRUE(fetched->content.has_value());
  EXPECT_EQ(fetched->content->size(), kPayloadBytes);
}

}  // namespace
