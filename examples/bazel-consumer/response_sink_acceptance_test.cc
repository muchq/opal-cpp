// Out-of-tree acceptance for the response body sink (issue #213): a consumer
// module pulling a blob payload off a generated server without the body
// landing in a response object, at both levels the runtime offers it.
//
// The transport level came first: `opal::http::BodySink` through
// `@opal_cpp//runtime:http`, driven by the caller, which is still what an
// operation whose payload the model does not mark `@streaming` gets. Slice 2
// added the generated level: a `@streaming` blob payload puts an
// `opal::http::BodyWriter` on the operation itself, and the generated code
// owns the sink's accept gate. Both are here because both are supported, and
// because the second is built on the first — a regression in the sink shows
// up in the generated path too.
//
// The transport underneath is `SocketHttpClient` both times — directly in the
// first half, and as what `RedirectorClient::Create` builds from an endpoint
// in the second. It inherits the default `SendStreaming`, delivering through
// the sink but buffering on the way, so what this file pins is the contract
// (the sink gets the bytes, the response does not) rather than the memory
// bound. The bound is `BeastHttpClient`'s,
// pinned in the runtime's own beast_client_test.cc, and a consumer gets it by
// injecting that transport instead. Keeping Beast out of this target is what
// lets it run behind a download-blocking proxy alongside the other
// socket-transport tests.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "acme/redirect/client.h"
#include "acme/redirect/server.h"
#include "opal/client/config.h"
#include "opal/core/error.h"
#include "opal/http/message.h"
#include "opal/http/socket_transport.h"
#include "opal/http/transport.h"

namespace {

using acme::redirect::DownloadErrors;
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

// A cheap order-sensitive fingerprint: a test that only counted bytes would
// pass on pieces delivered out of order or duplicated.
std::size_t Digest(std::string_view bytes) {
  std::size_t digest = 0;
  for (const char byte : bytes) digest = digest * 31 + static_cast<unsigned char>(byte);
  return digest;
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
  // Fetch's @streaming twin: the same bytes under the same rules, so the two
  // tests below compare the streamed and the buffered delivery of one
  // download rather than of two different ones. The server side is unchanged
  // by @streaming — the handler still returns the payload in the output.
  opal::Outcome<DownloadOutput> Download(const DownloadInput& input,
                                         const opal::server::RequestContext&) override {
    if (input.slug != "big") return NotFound(input.slug);
    return DownloadOutput{.etag = "\"big\"", .content = opal::Blob::FromString(payload_)};
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

  EXPECT_EQ(digest, Digest(payload_)) << "the bytes arrived, but not these bytes";
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

// The generated level (slice 2). The client is built from an endpoint alone —
// no transport injected, no sink assembled by hand — which is the whole point:
// a @streaming blob payload is streamed by the operation the generator wrote.
class StreamingPayloadAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(transport_.Start(server_.Handler()).ok());
    config_.endpoint = "http://127.0.0.1:" + std::to_string(transport_.port());
  }
  void TearDown() override { transport_.Stop(); }

  RedirectorClient Client() {
    auto client = RedirectorClient::Create(config_);
    EXPECT_TRUE(client.ok()) << client.error().message();
    return *std::move(client);
  }

  std::string payload_ = LargePayload();
  RedirectorServer server_{std::make_shared<DownloadHandler>(payload_)};
  opal::http::SocketHttpServer transport_;
  opal::ClientConfig config_;
};

TEST_F(StreamingPayloadAcceptanceTest, ThePayloadGoesToTheWriterAndTheMemberStaysEmpty) {
  RedirectorClient client = Client();

  std::size_t received = 0;
  std::size_t digest = 0;
  const auto downloaded =
      client.Download(DownloadInput{.slug = "big"}, [&](std::string_view piece) {
        received += piece.size();
        for (const char byte : piece) digest = digest * 31 + static_cast<unsigned char>(byte);
        return true;
      });

  ASSERT_TRUE(downloaded.ok()) << downloaded.error().message();
  EXPECT_EQ(received, payload_.size());
  EXPECT_EQ(digest, Digest(payload_)) << "the bytes arrived, but not these bytes";
  // The other bindings still deserialize — streaming the payload does not cost
  // the caller the rest of the output.
  EXPECT_EQ(downloaded->etag.value_or(""), "\"big\"");
  // Smithy requires @required on a streaming member, so `content` is a plain
  // Blob rather than an optional: streamed means empty, not absent.
  EXPECT_TRUE(downloaded->content.empty()) << "the payload was delivered twice";
}

TEST_F(StreamingPayloadAcceptanceTest, OmittingTheWriterBuffersThePayloadIntoTheMember) {
  // The writer is defaulted so the operation stays callable the ordinary way,
  // and a caller that does not want to stream is not forced to.
  RedirectorClient client = Client();

  const auto downloaded = client.Download(DownloadInput{.slug = "big"});
  ASSERT_TRUE(downloaded.ok()) << downloaded.error().message();
  EXPECT_EQ(downloaded->content.size(), payload_.size());
  EXPECT_EQ(Digest(downloaded->content.ToString()), Digest(payload_));
}

TEST_F(StreamingPayloadAcceptanceTest, AModeledErrorDeserializesInsteadOfReachingTheWriter) {
  // The accept gate the generator emits, from the outside: an error document
  // is not a payload, so it stays buffered where the typed-error path reads
  // it. Without the gate this call would return a 404-shaped success with an
  // error document in the caller's writer.
  RedirectorClient client = Client();

  bool wrote = false;
  const auto downloaded = client.Download(DownloadInput{.slug = "missing"}, [&](std::string_view) {
    wrote = true;
    return true;
  });

  ASSERT_FALSE(downloaded.ok());
  EXPECT_FALSE(wrote);
  const DownloadErrors modeled = DownloadErrors::FromError(downloaded.error());
  ASSERT_TRUE(modeled.is_no_such_slug()) << downloaded.error().message();
  EXPECT_EQ(modeled.as_no_such_slug().message, "no slug: missing");
}

TEST_F(StreamingPayloadAcceptanceTest, AWriterThatAbortsFailsTheCallWithoutRetrying) {
  // The caller hit its own limit (a disk quota, a length it refuses to
  // exceed). Retrying would deliver the same bytes again, so the failure is
  // not retryable and the backoff never runs.
  int sleeps = 0;
  config_.retry.sleep = [&](std::chrono::milliseconds) { ++sleeps; };
  RedirectorClient client = Client();

  std::size_t received = 0;
  const auto downloaded =
      client.Download(DownloadInput{.slug = "big"}, [&](std::string_view piece) {
        received += piece.size();
        return received <= payload_.size() / 2;
      });

  ASSERT_FALSE(downloaded.ok());
  EXPECT_FALSE(downloaded.error().retryable());
  EXPECT_EQ(sleeps, 0) << "the retry loop backed off for a body the caller refused";
}

}  // namespace
