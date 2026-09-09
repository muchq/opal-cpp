// The simpleRestJson @pattern-violation message assertion issue #48 called
// out as missing: the alloy conformance service models no @pattern, so this
// drives the roundtrip REST fixture (whose SinkId carries
// @pattern("^[A-Za-z0-9]+$")) and pins the exact ValidationException wire
// message — the same suite-exact text the jsonrpc2 protocol tests already
// assert for their protocol.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "example/roundtrip/rest/server.h"
#include "opal/json/json.h"

namespace example::roundtrip::rest {
namespace {

class RecordingHandler : public RoundTripRestHandler {
 public:
  opal::Outcome<DescribeSinkOutput> DescribeSink(const DescribeSinkInput&,
                                                 const opal::server::RequestContext&) override {
    ++calls;
    return DescribeSinkOutput{};
  }
  opal::Outcome<PutSinkOutput> PutSink(const PutSinkInput&,
                                       const opal::server::RequestContext&) override {
    ++calls;
    return PutSinkOutput{};
  }
  opal::Outcome<UploadAttachmentOutput> UploadAttachment(
      const UploadAttachmentInput&, const opal::server::RequestContext&) override {
    ++calls;
    return UploadAttachmentOutput{};
  }
  int calls = 0;
};

class PatternViolationTest : public testing::Test {
 protected:
  // Returns the single fieldList entry of a 400 ValidationException.
  opal::Document Reject(const std::string& target) {
    opal::http::HttpRequest request;
    request.method = "GET";
    request.target = target;
    const opal::http::HttpResponse response = server_.Handler()(request);
    EXPECT_EQ(response.status, 400) << response.body;
    EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "ValidationException");
    EXPECT_EQ(handler_->calls, 0);
    auto body = opal::json::Decode(response.body);
    EXPECT_TRUE(body.ok()) << response.body;
    const opal::Document* field_list = body->Find("fieldList");
    EXPECT_NE(field_list, nullptr) << response.body;
    EXPECT_EQ(field_list->as_list().size(), 1u) << response.body;
    return field_list->as_list()[0];
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RoundTripRestServer server_{handler_};
};

TEST_F(PatternViolationTest, PatternViolationReportsTheExactMessage) {
  const opal::Document failure = Reject("/sinks/bad!id");
  EXPECT_EQ(failure.Find("path")->as_string(), "/sinkId");
  EXPECT_EQ(failure.Find("message")->as_string(),
            "Value at '/sinkId' failed to satisfy constraint: Member must satisfy regular "
            "expression pattern: ^[A-Za-z0-9]+$");
}

TEST_F(PatternViolationTest, LengthViolationReportsTheExactMessage) {
  const opal::Document failure = Reject("/sinks/" + std::string(33, 'a'));
  EXPECT_EQ(failure.Find("path")->as_string(), "/sinkId");
  EXPECT_EQ(failure.Find("message")->as_string(),
            "Value with length 33 at '/sinkId' failed to satisfy constraint: Member must have "
            "length between 1 and 32, inclusive");
}

}  // namespace
}  // namespace example::roundtrip::rest
