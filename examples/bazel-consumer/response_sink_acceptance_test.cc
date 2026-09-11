// Out-of-tree acceptance for the response body sink (issue #213): a consumer
// module reaching `opal::http::BodySink` through `@opal_cpp//runtime:http`
// and pulling a blob payload off a generated server without the body landing
// in a response object.
//
// This is the shape a consumer has today, and deliberately so: the sink lives
// on the transport, so a caller drives it directly. Slice 2 of #213 is what
// puts a `@streaming` blob behind a generated method; until then this test is
// the record of what the runtime alone gives you.
//
// The transport here is `SocketHttpClient`, which inherits the default
// `SendStreaming` — it delivers through the sink but buffers on the way, so
// what this test pins is the contract (the sink gets the bytes, the response
// does not) rather than the memory bound. The bound is `BeastHttpClient`'s,
// pinned in the runtime's own beast_client_test.cc, and a consumer gets it by
// injecting that transport instead. Keeping Beast out of this target is what
// lets it run behind a download-blocking proxy alongside the other
// socket-transport tests.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "acme/redirect/client.h"
#include "acme/redirect/server.h"
#include "opal/core/error.h"
#include "opal/http/message.h"
#include "opal/http/socket_transport.h"
#include "opal/http/transport.h"

namespace {

using acme::redirect::FetchInput;
using acme::redirect::FetchOutput;
using acme::redirect::NoSuchSlug;
using acme::redirect::ProbeInput;
using acme::redirect::ProbeOutput;
using acme::redirect::RedirectorHandler;
using acme::redirect::RedirectorServer;
using acme::redirect::ResolveDynamicInput;
using acme::redirect::ResolveDynamicOutput;
using acme::redirect::ResolveInput;
using acme::redirect::ResolveOutput;

// A payload big enough that nobody would want it in memory twice — the case
// the sink exists for. The content is a repeating pattern rather than one
// character so a test can tell a truncated delivery from a short one.
std::string LargePayload() {
  std::string payload;
  payload.reserve(512 * 1024);
  while (payload.size() < 512 * 1024) {
    payload += "the quick brown fox jumps over the lazy dog\n";
  }
  return payload;
}

class DownloadHandler final : public RedirectorHandler {
 public:
  explicit DownloadHandler(std::string payload) : payload_(std::move(payload)) {}

  opal::Outcome<FetchOutput> Fetch(const FetchInput& input,
                                   const opal::server::RequestContext&) override {
    if (input.slug != "big") return NotFound(input.slug);
    return FetchOutput{
        .status = 200, .etag = "\"big\"", .content = opal::Blob::FromString(payload_)};
  }
  opal::Outcome<ProbeOutput> Probe(const ProbeInput&,
                                   const opal::server::RequestContext&) override {
    return ProbeOutput{.etag = "\"big\"", .content = opal::Blob::FromString(payload_)};
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

  std::string payload_;
};

class ResponseSinkAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(transport_.Start(server_.Handler()).ok()); }
  void TearDown() override { transport_.Stop(); }

  opal::http::HttpRequest Get(const std::string& slug) const {
    opal::http::HttpRequest request;
    request.method = "GET";
    request.target = "/c/" + slug;
    return request;
  }

  std::string payload_ = LargePayload();
  RedirectorServer server_{std::make_shared<DownloadHandler>(payload_)};
  opal::http::SocketHttpServer transport_;
};

TEST_F(ResponseSinkAcceptanceTest, ABlobPayloadArrivesThroughTheSinkAndNotInTheResponse) {
  opal::http::SocketHttpClient client("127.0.0.1", transport_.port());

  // What a real consumer does with the pieces: hand them straight to
  // something that consumes bytes — a file, a hash, a parser. Nothing here
  // keeps the payload, only its length and digest.
  std::size_t received = 0;
  std::size_t digest = 0;
  const opal::http::BodySink to_consumer{
      .accept = [](int status, const opal::http::Headers&) { return status == 200; },
      .write =
          [&](std::string_view piece) {
            received += piece.size();
            for (const char byte : piece) digest = digest * 31 + static_cast<unsigned char>(byte);
            return true;
          },
  };

  const auto response = client.SendStreaming(Get("big"), to_consumer);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("etag").value_or(""), "\"big\"");
  EXPECT_EQ(received, payload_.size());
  EXPECT_TRUE(response->body.empty()) << "the payload was delivered twice";

  std::size_t expected = 0;
  for (const char byte : payload_) expected = expected * 31 + static_cast<unsigned char>(byte);
  EXPECT_EQ(digest, expected) << "the bytes arrived, but not these bytes";
}

TEST_F(ResponseSinkAcceptanceTest, AModeledErrorIsLeftWhereTheClientLooksForIt) {
  // The reason accept() is asked per response: a sink that takes payloads
  // must not swallow the error document that the generated client's own
  // deserializer needs to turn a 404 into a NoSuchSlug.
  opal::http::SocketHttpClient client("127.0.0.1", transport_.port());

  bool wrote = false;
  const opal::http::BodySink payloads_only{
      .accept = [](int status, const opal::http::Headers&) { return status == 200; },
      .write =
          [&](std::string_view) {
            wrote = true;
            return true;
          },
  };

  const auto response = client.SendStreaming(Get("missing"), payloads_only);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 404);
  EXPECT_FALSE(wrote);
  // The modeled member is still in the document, which is what the generated
  // client's deserializer reads to build the typed NoSuchSlug detail. Had the
  // sink taken this response, it would have read an empty body instead.
  EXPECT_NE(response->body.find("no slug: missing"), std::string::npos) << response->body;
}

}  // namespace
