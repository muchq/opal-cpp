// Wire-level checks for @httpApiKeyAuth(in: "query"): the generated client
// appends the key to the target, taking the '?' branch when the operation
// produced no query string and the '&' branch when it did — and the key
// itself is percent-encoded on the way.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "example/roundtrip/rest/client.h"
#include "smithy/client/config.h"
#include "smithy/http/transport.h"

namespace example::roundtrip::rest {
namespace {

class CapturingTransport final : public opal::http::HttpClient {
 public:
  opal::Outcome<opal::http::HttpResponse> Send(const opal::http::HttpRequest& request) override {
    last_request = request;
    return opal::http::HttpResponse{200, {}, ""};
  }

  opal::http::HttpRequest last_request;
};

RoundTripRestClient MakeClient(std::shared_ptr<CapturingTransport> transport) {
  opal::ClientConfig config;
  config.http_client = std::move(transport);
  config.api_key = [] { return std::string("k&e y"); };  // needs encoding
  return *RoundTripRestClient::Create(std::move(config));
}

TEST(ApiKeyQueryTest, StartsTheQueryStringWhenThereIsNone) {
  auto transport = std::make_shared<CapturingTransport>();
  auto client = MakeClient(transport);
  (void)client.DescribeSink(DescribeSinkInput{.sinkId = "s1"});
  EXPECT_EQ(transport->last_request.target, "/sinks/s1?api-key=k%26e%20y");
}

TEST(ApiKeyQueryTest, AppendsToAnExistingQueryString) {
  auto transport = std::make_shared<CapturingTransport>();
  auto client = MakeClient(transport);
  (void)client.PutSink(PutSinkInput{.sinkId = "s1", .tag = "prod"});
  // limit is @required, so the (default-zero) value always rides along.
  EXPECT_EQ(transport->last_request.target, "/sinks/s1?limit=0&tag=prod&api-key=k%26e%20y");
}

TEST(ApiKeyQueryTest, AbsentProviderLeavesTheTargetAlone) {
  auto transport = std::make_shared<CapturingTransport>();
  opal::ClientConfig config;
  config.http_client = transport;
  auto client = RoundTripRestClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  (void)client->DescribeSink(DescribeSinkInput{.sinkId = "s1"});
  EXPECT_EQ(transport->last_request.target, "/sinks/s1");
}

}  // namespace
}  // namespace example::roundtrip::rest
