// Union x rpcv2Cbor conformance (issue #48): until this suite, union
// round-tripping was only pinned for simpleRestJson — the cbor cell relied on
// the seeded random integration tests, which flip a coin on whether the union
// appears at all and can only prove serde self-consistency, not wire
// correctness. These tests pin the wire subdocument for every SinkChoice
// variant deterministically, in all four directions: client encode, client
// decode, server decode, server encode — plus the reject cells (empty,
// multi-member, unknown-member, null-member unions) and the __type
// discriminator tolerance on both sides.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "example/roundtrip/rpc/client.h"
#include "example/roundtrip/rpc/server.h"
#include "opal/cbor/cbor.h"
#include "opal/client/config.h"
#include "opal/core/document.h"
#include "opal/testing/protocol_test.h"

namespace example::roundtrip::rpc {
namespace {

opal::Document TextChoiceDoc(const std::string& text) {
  opal::DocumentMap map;
  map.emplace("text", opal::Document(text));
  return opal::Document(std::move(map));
}

opal::Document CountChoiceDoc(std::int64_t count) {
  opal::DocumentMap map;
  map.emplace("count", opal::Document(count));
  return opal::Document(std::move(map));
}

opal::Document NestedChoiceDoc(const std::string& label, std::int64_t depth) {
  opal::DocumentMap nested;
  nested.emplace("label", opal::Document(label));
  nested.emplace("depth", opal::Document(depth));
  opal::DocumentMap map;
  map.emplace("nested", opal::Document(std::move(nested)));
  return opal::Document(std::move(map));
}

// A wire body for PutSinkRpc carrying only the members the union cell needs.
std::string BodyWithChoice(const opal::Document& choice) {
  opal::DocumentMap sink;
  sink.emplace("name", opal::Document("n"));
  sink.emplace("choice", choice);
  opal::DocumentMap body;
  body.emplace("sinkId", opal::Document("s1"));
  body.emplace("sink", opal::Document(std::move(sink)));
  return opal::cbor::Encode(opal::Document(std::move(body))).ToString();
}

PutSinkRpcInput InputWithChoice(SinkChoice choice) {
  KitchenSink sink;
  sink.name = "n";
  sink.choice = std::move(choice);
  PutSinkRpcInput input;
  input.sinkId = "s1";
  input.sink = std::move(sink);
  return input;
}

// The choice subdocument of a captured PutSinkRpc request body.
opal::Document ChoiceOf(const std::string& wire_body) {
  auto doc = opal::cbor::Decode(opal::Blob::FromString(wire_body));
  EXPECT_TRUE(doc.ok());
  if (!doc.ok()) return opal::Document(nullptr);
  const opal::Document* sink = doc->Find("sink");
  EXPECT_NE(sink, nullptr);
  if (sink == nullptr) return opal::Document(nullptr);
  const opal::Document* choice = sink->Find("choice");
  EXPECT_NE(choice, nullptr);
  return choice == nullptr ? opal::Document(nullptr) : *choice;
}

class UnionCborClientTest : public testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_shared<opal::testing::CapturingTransport>();
    opal::DocumentMap ok_body;
    ok_body.emplace("sinkId", opal::Document("s1"));
    transport_->next_response = opal::http::HttpResponse{
        200, {}, opal::cbor::Encode(opal::Document(std::move(ok_body))).ToString()};
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = transport_;
    auto client = RoundTripRpcClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<RoundTripRpcClient>(std::move(*client));
  }

  std::shared_ptr<opal::testing::CapturingTransport> transport_;
  std::unique_ptr<RoundTripRpcClient> client_;
};

TEST_F(UnionCborClientTest, EncodesEachVariantAsASingleMemberMap) {
  const struct {
    SinkChoice choice;
    opal::Document expected;
  } cells[] = {
      {SinkChoice::FromText("wire text"), TextChoiceDoc("wire text")},
      {SinkChoice::FromCount(-7), CountChoiceDoc(-7)},
      {SinkChoice::FromNested([] {
         NestedConfig nested;
         nested.label = "L";
         nested.depth = 3;
         return nested;
       }()),
       NestedChoiceDoc("L", 3)},
  };
  for (const auto& cell : cells) {
    ASSERT_TRUE(client_->PutSinkRpc(InputWithChoice(cell.choice)).ok());
    const opal::Document choice = ChoiceOf(transport_->last_request.body);
    ASSERT_TRUE(choice.is_map());
    EXPECT_EQ(choice.as_map().size(), 1u) << "a union must serialize exactly one member";
    EXPECT_EQ(choice, cell.expected);
    EXPECT_EQ(transport_->last_request.headers.Get("smithy-protocol"), "rpc-v2-cbor");
  }
}

TEST_F(UnionCborClientTest, DecodesEachVariantFromAResponse) {
  const struct {
    opal::Document wire;
    SinkChoice expected;
  } cells[] = {
      {TextChoiceDoc("from server"), SinkChoice::FromText("from server")},
      {CountChoiceDoc(42), SinkChoice::FromCount(42)},
      {NestedChoiceDoc("deep", 9), SinkChoice::FromNested([] {
         NestedConfig nested;
         nested.label = "deep";
         nested.depth = 9;
         return nested;
       }())},
  };
  for (const auto& cell : cells) {
    transport_->next_response = opal::http::HttpResponse{200, {}, BodyWithChoice(cell.wire)};
    const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
    ASSERT_TRUE(outcome.ok()) << outcome.error().message();
    ASSERT_TRUE(outcome->sink.has_value());
    ASSERT_TRUE(outcome->sink->choice.has_value());
    EXPECT_EQ(*outcome->sink->choice, cell.expected);
  }
}

TEST_F(UnionCborClientTest, RejectsInvalidUnionsInResponses) {
  // Each cell pins its diagnosis, not just the rejection: a union declined
  // for the wrong reason (a generic parse failure, a missing-field error)
  // would mask the exactly-one-member rule this test defends.
  const struct {
    opal::Document wire;
    const char* why;
    const char* diagnosis;
  } cells[] = {
      {opal::Document(opal::DocumentMap{}), "empty union", "expected exactly one union member"},
      {[] {
         opal::DocumentMap map;
         map.emplace("text", opal::Document("a"));
         map.emplace("count", opal::Document(std::int64_t{1}));
         return opal::Document(std::move(map));
       }(),
       "two members set", "expected exactly one union member"},
      {[] {
         opal::DocumentMap map;
         map.emplace("futureMember", opal::Document(std::int64_t{1}));
         return opal::Document(std::move(map));
       }(),
       "unknown member", "unknown or missing union member"},
      {[] {
         opal::DocumentMap map;
         map.emplace("text", opal::Document(nullptr));
         return opal::Document(std::move(map));
       }(),
       "null member", "unknown or missing union member"},
      {opal::Document("not a map"), "non-map union", "expected a map on the wire"},
  };
  for (const auto& cell : cells) {
    transport_->next_response = opal::http::HttpResponse{200, {}, BodyWithChoice(cell.wire)};
    const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
    ASSERT_FALSE(outcome.ok()) << cell.why;
    EXPECT_NE(outcome.error().message().find(cell.diagnosis), std::string::npos)
        << cell.why << ": " << outcome.error().message();
  }
}

TEST_F(UnionCborClientTest, ToleratesATypeDiscriminatorNextToTheMember) {
  opal::DocumentMap map;
  map.emplace("__type", opal::Document("example.roundtrip#SinkChoice"));
  map.emplace("text", opal::Document("discriminated"));
  transport_->next_response =
      opal::http::HttpResponse{200, {}, BodyWithChoice(opal::Document(std::move(map)))};
  const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
  ASSERT_TRUE(outcome.ok()) << outcome.error().message();
  EXPECT_EQ(*outcome->sink->choice, SinkChoice::FromText("discriminated"));
}

// --- Server side: the same cells through the generated request path -------

class RecordingHandler : public RoundTripRpcHandler {
 public:
  opal::Outcome<PutSinkRpcOutput> PutSinkRpc(const PutSinkRpcInput& input,
                                             const opal::server::RequestContext&) override {
    last = input;
    PutSinkRpcOutput output;
    output.sinkId = input.sinkId;
    output.sink = input.sink;  // echo, so the response leg is exercised too
    return output;
  }
  opal::Outcome<PingOutput> Ping(const PingInput&, const opal::server::RequestContext&) override {
    return PingOutput{};
  }
  std::optional<PutSinkRpcInput> last;
};

class UnionCborServerTest : public testing::Test {
 protected:
  opal::http::HttpResponse Send(const std::string& body) {
    return server_.Handler()(opal::testing::Rpcv2CborRequest("RoundTripRpc", "PutSinkRpc", body));
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RoundTripRpcServer server_{handler_};
};

TEST_F(UnionCborServerTest, DecodesEachVariantAndEchoesItBack) {
  const struct {
    opal::Document wire;
    SinkChoice expected;
  } cells[] = {
      {TextChoiceDoc("to server"), SinkChoice::FromText("to server")},
      {CountChoiceDoc(-1), SinkChoice::FromCount(-1)},
      {NestedChoiceDoc("srv", 2), SinkChoice::FromNested([] {
         NestedConfig nested;
         nested.label = "srv";
         nested.depth = 2;
         return nested;
       }())},
  };
  for (const auto& cell : cells) {
    const auto response = Send(BodyWithChoice(cell.wire));
    ASSERT_EQ(response.status, 200) << response.body;
    ASSERT_TRUE(handler_->last.has_value());
    ASSERT_TRUE(handler_->last->sink.has_value() && handler_->last->sink->choice.has_value());
    EXPECT_EQ(*handler_->last->sink->choice, cell.expected);

    // The echoed response body carries the identical union subdocument.
    EXPECT_EQ(ChoiceOf(response.body), cell.wire);
    handler_->last.reset();
  }
}

TEST_F(UnionCborServerTest, RejectsInvalidUnionsBeforeTheHandler) {
  const struct {
    opal::Document wire;
    const char* diagnosis;
  } cells[] = {
      {opal::Document(opal::DocumentMap{}), "expected exactly one union member"},
      {[] {
         opal::DocumentMap map;
         map.emplace("text", opal::Document("a"));
         map.emplace("count", opal::Document(std::int64_t{1}));
         return opal::Document(std::move(map));
       }(),
       "expected exactly one union member"},
      {[] {
         opal::DocumentMap map;
         map.emplace("futureMember", opal::Document(std::int64_t{1}));
         return opal::Document(std::move(map));
       }(),
       "unknown or missing union member"},
  };
  for (const auto& cell : cells) {
    const auto response = Send(BodyWithChoice(cell.wire));
    EXPECT_EQ(response.status, 400) << response.body;
    EXPECT_FALSE(handler_->last.has_value());
    // The 400's error body names the union rule that was violated.
    auto body = opal::cbor::Decode(opal::Blob::FromString(response.body));
    ASSERT_TRUE(body.ok());
    const opal::Document* message = body->Find("message");
    ASSERT_NE(message, nullptr);
    EXPECT_NE(message->as_string().find(cell.diagnosis), std::string::npos) << message->as_string();
  }
}

}  // namespace
}  // namespace example::roundtrip::rpc
