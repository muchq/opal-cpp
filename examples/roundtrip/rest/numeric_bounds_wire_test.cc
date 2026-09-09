// Wire-level pins for the issue-#109 narrowing fixes, across the module
// boundary (generated code ↔ runtime) in both directions:
//
//  - intEnum members share the int32 bounds check with Integer members — a
//    wire value of 2^32+2 must fail the parse, never alias onto a valid
//    enumerator via the truncating cast.
//  - float members reject finite doubles beyond float range — the raw
//    static_cast would be UB ([conv.double], UBSan float-cast-overflow).
//  - Servers additionally validate intEnum membership (ValidationException,
//    string-enum policy) while clients keep unknown-but-in-range values for
//    forward compatibility. That asymmetry is deliberate and pinned here.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "example/roundtrip/rest/client.h"
#include "example/roundtrip/rest/serde.h"
#include "example/roundtrip/rest/server.h"
#include "opal/client/config.h"
#include "opal/core/document.h"
#include "opal/http/transport.h"
#include "opal/json/json.h"

namespace example::roundtrip::rest {
namespace {

// --- Direct serde boundary: hostile Documents into the generated parser. ---

opal::Document SinkDoc(const char* member, opal::Document value) {
  opal::DocumentMap map;
  map.emplace("name", opal::Document(std::string("n")));
  map.emplace(member, std::move(value));
  return opal::Document(std::move(map));
}

TEST(NumericBoundsSerdeTest, IntEnumBeyondInt32FailsInsteadOfAliasing) {
  // 2^32+2 truncates to 2 (a valid Weight) under the old cast.
  auto sink = DeserializeKitchenSink(SinkDoc("weight", opal::Document(std::int64_t{4294967298})));
  ASSERT_FALSE(sink.ok());
  EXPECT_EQ(sink.error().message(), "KitchenSink.weight: value out of range");
}

TEST(NumericBoundsSerdeTest, IntEnumKeepsUnknownInRangeValues) {
  // Model evolution: a value the client's model doesn't know yet still
  // parses (matching string enums' unknown handling); only servers reject
  // it, via validation.
  auto sink = DeserializeKitchenSink(SinkDoc("weight", opal::Document(std::int64_t{7})));
  ASSERT_TRUE(sink.ok()) << sink.error().message();
  ASSERT_TRUE(sink->weight.has_value());
  EXPECT_EQ(*sink->weight, static_cast<Weight>(7));
}

TEST(NumericBoundsSerdeTest, FloatBeyondRangeFailsInsteadOfUb) {
  auto sink = DeserializeKitchenSink(SinkDoc("ratio", opal::Document(1e300)));
  ASSERT_FALSE(sink.ok());
  EXPECT_EQ(sink.error().message(), "KitchenSink.ratio: value out of range");
}

TEST(NumericBoundsSerdeTest, FloatEdgeAndNonFiniteValuesStillParse) {
  // The exact float maximum is in range...
  const double float_max = static_cast<double>(std::numeric_limits<float>::max());
  auto edge = DeserializeKitchenSink(SinkDoc("ratio", opal::Document(float_max)));
  ASSERT_TRUE(edge.ok()) << edge.error().message();
  EXPECT_EQ(*edge->ratio, std::numeric_limits<float>::max());
  // ...as is what a peer's shortest-round-trip printer (or our own
  // FormatFloat) puts on the wire for float max — a double slightly above
  // FLT_MAX that still rounds back to it.
  auto shortest = DeserializeKitchenSink(SinkDoc("ratio", opal::Document(3.4028235e38)));
  ASSERT_TRUE(shortest.ok()) << shortest.error().message();
  EXPECT_EQ(*shortest->ratio, std::numeric_limits<float>::max());
  // ...and the Smithy non-finite spellings narrow losslessly, never caught
  // in the overflow net.
  auto inf = DeserializeKitchenSink(SinkDoc("ratio", opal::Document(std::string("-Infinity"))));
  ASSERT_TRUE(inf.ok()) << inf.error().message();
  EXPECT_TRUE(std::isinf(*inf->ratio));
  EXPECT_LT(*inf->ratio, 0.0F);
}

// --- Server over the wire: hostile JSON bodies into the generated router. ---

class RecordingHandler : public RoundTripRestHandler {
 public:
  opal::Outcome<DescribeSinkOutput> DescribeSink(const DescribeSinkInput&,
                                                 const opal::server::RequestContext&) override {
    ++calls;
    return DescribeSinkOutput{};
  }
  opal::Outcome<PutSinkOutput> PutSink(const PutSinkInput& input,
                                       const opal::server::RequestContext&) override {
    ++calls;
    last_sink = input.sink;
    return PutSinkOutput{.sinkId = "s1"};
  }
  opal::Outcome<UploadAttachmentOutput> UploadAttachment(
      const UploadAttachmentInput&, const opal::server::RequestContext&) override {
    ++calls;
    return UploadAttachmentOutput{};
  }
  int calls = 0;
  std::optional<KitchenSink> last_sink;
};

class NumericBoundsServerTest : public testing::Test {
 protected:
  opal::http::HttpResponse PutSinkBody(const std::string& body) {
    opal::http::HttpRequest request;
    request.method = "PUT";
    request.target = "/sinks/s1?limit=5";
    request.headers.Set("content-type", "application/json");
    request.headers.Set("x-sink-created", "Sun, 06 Nov 1994 08:49:37 GMT");
    request.body = body;
    return server_.Handler()(request);
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RoundTripRestServer server_{handler_};
};

TEST_F(NumericBoundsServerTest, IntEnumBeyondInt32IsRejectedBeforeTheHandler) {
  const auto response = PutSinkBody(R"({"sink":{"name":"n","weight":4294967298}})");
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(NumericBoundsServerTest, FloatBeyondRangeIsRejectedBeforeTheHandler) {
  const auto response = PutSinkBody(R"({"sink":{"name":"n","ratio":1e300}})");
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(NumericBoundsServerTest, UnknownIntEnumValueFailsValidationWithTheSuiteMessage) {
  const auto response = PutSinkBody(R"({"sink":{"name":"n","weight":3}})");
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "ValidationException");
  EXPECT_EQ(handler_->calls, 0);
  auto body = opal::json::Decode(response.body);
  ASSERT_TRUE(body.ok()) << response.body;
  const opal::Document* field_list = body->Find("fieldList");
  ASSERT_NE(field_list, nullptr) << response.body;
  ASSERT_EQ(field_list->as_list().size(), 1u) << response.body;
  const auto& failure = field_list->as_list()[0];
  EXPECT_EQ(failure.Find("path")->as_string(), "/sink/weight");
  EXPECT_EQ(failure.Find("message")->as_string(),
            "Value at '/sink/weight' failed to satisfy constraint: Member must satisfy enum value "
            "set: [1, 2]");
}

TEST_F(NumericBoundsServerTest, ValidValuesReachTheHandlerIntact) {
  const auto response = PutSinkBody(R"({"sink":{"name":"n","weight":2,"ratio":2.5}})");
  EXPECT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(handler_->calls, 1);
  ASSERT_TRUE(handler_->last_sink.has_value());
  EXPECT_EQ(handler_->last_sink->weight, Weight::kHeavy);
  EXPECT_EQ(handler_->last_sink->ratio, 2.5F);
}

// --- Client over the wire: hostile server responses into the generated
// client. ---

class CannedTransport final : public opal::http::HttpClient {
 public:
  explicit CannedTransport(std::string body) : body_(std::move(body)) {}

  opal::Outcome<opal::http::HttpResponse> Send(const opal::http::HttpRequest&) override {
    opal::http::HttpResponse response{200, {}, body_};
    response.headers.Set("content-type", "application/json");
    return response;
  }

 private:
  std::string body_;
};

opal::Outcome<DescribeSinkOutput> Describe(const std::string& body) {
  opal::ClientConfig config;
  config.http_client = std::make_shared<CannedTransport>(body);
  auto client = RoundTripRestClient::Create(std::move(config));
  if (!client.ok()) {
    // Not ASSERT (void-only): fail the test and hand back the creation
    // error rather than dereferencing a failed Outcome.
    ADD_FAILURE() << client.error().message();
    return client.error();
  }
  return client->DescribeSink(DescribeSinkInput{.sinkId = "s1"});
}

TEST(NumericBoundsClientTest, IntEnumBeyondInt32FailsTheResponseParse) {
  const auto outcome = Describe(R"({"sink":{"name":"n","weight":4294967298}})");
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().message(), "KitchenSink.weight: value out of range");
}

TEST(NumericBoundsClientTest, FloatBeyondRangeFailsTheResponseParse) {
  const auto outcome = Describe(R"({"sink":{"name":"n","ratio":1e300}})");
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().message(), "KitchenSink.ratio: value out of range");
}

TEST(NumericBoundsClientTest, UnknownInRangeIntEnumValueSurvivesForForwardCompat) {
  const auto outcome = Describe(R"({"sink":{"name":"n","weight":7}})");
  ASSERT_TRUE(outcome.ok()) << outcome.error().message();
  ASSERT_TRUE(outcome->sink.has_value());
  EXPECT_EQ(outcome->sink->weight, static_cast<Weight>(7));
}

}  // namespace
}  // namespace example::roundtrip::rest
