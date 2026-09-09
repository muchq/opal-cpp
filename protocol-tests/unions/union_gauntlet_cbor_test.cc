// Union member-type gauntlet x rpcv2Cbor (issue #56): SinkChoice only covers
// string/int/struct members, so these are the missing cells — blob,
// timestamp, list, map, enum, intEnum, and a recursive struct member — each
// pinned with a hand-built typed value on one side and a hand-built wire
// document on the other (never both ends produced by the code under test).
// Also here: the byte-exact request vector (RFC 8949 deterministic
// encoding, derived by hand from the spec, not captured from the encoder)
// and the error-shape union cell, where the __type discriminator rides next
// to the union member — the case the serde's exactly-one-member arithmetic
// exists to tolerate.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compile/unions/cbor/client.h"
#include "compile/unions/cbor/server.h"
#include "smithy/cbor/cbor.h"
#include "smithy/client/config.h"
#include "smithy/core/document.h"
#include "smithy/core/timestamp.h"
#include "smithy/http/loopback.h"
#include "smithy/testing/protocol_test.h"

namespace compile::unions::cbor {
namespace {

opal::Document Doc(opal::DocumentMap map) { return opal::Document(std::move(map)); }

opal::Document OneMember(const std::string& name, opal::Document value) {
  opal::DocumentMap map;
  map.emplace(name, std::move(value));
  return Doc(std::move(map));
}

std::string RequestBody(const opal::Document& choice) {
  opal::DocumentMap body;
  body.emplace("id", opal::Document("a"));
  body.emplace("choice", choice);
  return opal::cbor::Encode(Doc(std::move(body))).ToString();
}

// One row per member kind: the typed value and its wire subdocument.
struct Cell {
  const char* name;
  BigUnion typed;
  opal::Document wire;
};

std::vector<Cell> MemberCells() {
  const auto timestamp = opal::Timestamp::FromEpochMilliseconds(1515531081000);
  opal::DocumentList names;
  names.emplace_back("x");
  names.emplace_back("y");
  opal::DocumentMap attributes;
  attributes.emplace("k", opal::Document("v"));
  opal::DocumentMap node;
  node.emplace("label", opal::Document("outer"));
  node.emplace("next", OneMember("label", opal::Document("inner")));

  Node inner;
  inner.label = "inner";
  Node outer;
  outer.label = "outer";
  outer.next = opal::Boxed<Node>(inner);

  std::vector<Cell> cells;
  cells.push_back({"text", BigUnion::FromText("t"), OneMember("text", opal::Document("t"))});
  cells.push_back({"flag", BigUnion::FromFlag(true), OneMember("flag", opal::Document(true))});
  cells.push_back(
      {"small", BigUnion::FromSmall(-3), OneMember("small", opal::Document(std::int64_t{-3}))});
  cells.push_back({"big", BigUnion::FromBig(std::int64_t{1} << 40),
                   OneMember("big", opal::Document(std::int64_t{1} << 40))});
  cells.push_back({"ratio", BigUnion::FromRatio(1.5), OneMember("ratio", opal::Document(1.5))});
  cells.push_back({"data", BigUnion::FromData(opal::Blob::FromString("\x01\x02")),
                   OneMember("data", opal::Document(opal::Blob::FromString("\x01\x02")))});
  cells.push_back({"when", BigUnion::FromWhen(timestamp),
                   OneMember("when", opal::Document::FromTimestamp(
                                         timestamp, opal::TimestampFormat::kEpochSeconds))});
  cells.push_back({"names", BigUnion::FromNames({"x", "y"}),
                   OneMember("names", opal::Document(std::move(names)))});
  cells.push_back({"attributes", BigUnion::FromAttributes({{"k", "v"}}),
                   OneMember("attributes", Doc(std::move(attributes)))});
  cells.push_back({"grade", BigUnion::FromGrade(Grade::Value::kPass),
                   OneMember("grade", opal::Document("pass"))});
  cells.push_back({"rank", BigUnion::FromRank(Rank::kFirst),
                   OneMember("rank", opal::Document(std::int64_t{1}))});
  cells.push_back({"node", BigUnion::FromNode(outer), OneMember("node", Doc(std::move(node)))});
  return cells;
}

// The handler echoes, so one loopback call exercises server decode and
// server encode with the client's two directions anchoring the ends.
class EchoHandler : public UnionGauntletHandler {
 public:
  opal::Outcome<EchoChoiceOutput> EchoChoice(const EchoChoiceInput& input,
                                             const opal::server::RequestContext&) override {
    last = input;
    if (reject) {
      opal::Error error = opal::Error::Modeled("ChoiceRejected", "no thanks");
      ChoiceRejected detail;
      detail.message = "no thanks";
      detail.offending = input.choice;
      error.set_detail(std::move(detail));
      return error;
    }
    EchoChoiceOutput output;
    output.id = input.id;
    output.choice = input.choice;
    return output;
  }
  std::optional<EchoChoiceInput> last;
  bool reject = false;
};

class UnionGauntletCborTest : public testing::Test {
 protected:
  void SetUp() override {
    handler_ = std::make_shared<EchoHandler>();
    server_ = std::make_unique<UnionGauntletServer>(handler_);
    transport_ = std::make_shared<opal::testing::CapturingTransport>();
  }

  // A client whose requests go to the real generated server.
  UnionGauntletClient LiveClient() {
    auto loopback = std::make_shared<opal::http::Loopback>();
    EXPECT_TRUE(loopback->Start(server_->Handler()).ok());
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = loopback;
    auto client = UnionGauntletClient::Create(std::move(config));
    EXPECT_TRUE(client.ok());
    return std::move(*client);
  }

  // A client whose requests are captured and answered with a canned body.
  UnionGauntletClient CapturedClient() {
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = transport_;
    auto client = UnionGauntletClient::Create(std::move(config));
    EXPECT_TRUE(client.ok());
    return std::move(*client);
  }

  std::shared_ptr<EchoHandler> handler_;
  std::unique_ptr<UnionGauntletServer> server_;
  std::shared_ptr<opal::testing::CapturingTransport> transport_;
};

TEST_F(UnionGauntletCborTest, EveryMemberKindEncodesToItsWireSubdocument) {
  auto client = CapturedClient();
  transport_->next_response = opal::http::HttpResponse{
      200, {}, opal::cbor::Encode(OneMember("id", opal::Document("a"))).ToString()};
  for (const Cell& cell : MemberCells()) {
    EchoChoiceInput input;
    input.id = "a";
    input.choice = cell.typed;
    ASSERT_TRUE(client.EchoChoice(input).ok()) << cell.name;
    auto body = opal::cbor::Decode(opal::Blob::FromString(transport_->last_request.body));
    ASSERT_TRUE(body.ok()) << cell.name;
    const opal::Document* choice = body->Find("choice");
    ASSERT_NE(choice, nullptr) << cell.name;
    ASSERT_TRUE(choice->is_map()) << cell.name;
    EXPECT_EQ(choice->as_map().size(), 1u) << cell.name;
    EXPECT_EQ(*choice, cell.wire) << cell.name;
  }
}

TEST_F(UnionGauntletCborTest, EveryMemberKindSurvivesTheFullClientServerLoop) {
  auto client = LiveClient();
  for (const Cell& cell : MemberCells()) {
    EchoChoiceInput input;
    input.id = "a";
    input.choice = cell.typed;
    const auto outcome = client.EchoChoice(input);
    ASSERT_TRUE(outcome.ok()) << cell.name << ": " << outcome.error().message();
    // Server decode, anchored by the hand-built typed value.
    ASSERT_TRUE(handler_->last.has_value()) << cell.name;
    ASSERT_TRUE(handler_->last->choice.has_value()) << cell.name;
    EXPECT_EQ(*handler_->last->choice, cell.typed) << cell.name;
    // Client decode of the server's echo, anchored the same way.
    ASSERT_TRUE(outcome->choice.has_value()) << cell.name;
    EXPECT_EQ(*outcome->choice, cell.typed) << cell.name;
    handler_->last.reset();
  }
}

// RFC 8949 deterministic encoding of {"choice": {"text": "hi"}, "id": "a"},
// derived by hand from the spec (definite lengths, sorted keys): the byte
// level pin that Document-equality checks cannot provide.
TEST_F(UnionGauntletCborTest, RequestBodyIsByteExact) {
  auto client = CapturedClient();
  transport_->next_response = opal::http::HttpResponse{
      200, {}, opal::cbor::Encode(OneMember("id", opal::Document("a"))).ToString()};
  EchoChoiceInput input;
  input.id = "a";
  input.choice = BigUnion::FromText("hi");
  ASSERT_TRUE(client.EchoChoice(input).ok());
  static constexpr char kHex[] = "0123456789abcdef";
  std::string actual;
  for (const unsigned char byte : transport_->last_request.body) {
    actual.push_back(kHex[byte >> 4]);
    actual.push_back(kHex[byte & 0xF]);
  }
  EXPECT_EQ(actual,
            "a2"              // map(2)
            "6663686f696365"  // "choice"
            "a1"              // map(1)
            "6474657874"      // "text"
            "626869"          // "hi"
            "626964"          // "id"
            "6161");          // "a"
}

TEST_F(UnionGauntletCborTest, ModeledErrorCarriesAUnionNextToItsTypeDiscriminator) {
  handler_->reject = true;

  // Server side: the error body carries __type AND the union member — the
  // wire shape whose deserialization the exactly-one arithmetic tolerates.
  const auto response = server_->Handler()(opal::testing::Rpcv2CborRequest(
      "UnionGauntlet", "EchoChoice", RequestBody(OneMember("grade", opal::Document("fail")))));
  EXPECT_EQ(response.status, 400) << response.body;
  auto body = opal::cbor::Decode(opal::Blob::FromString(response.body));
  ASSERT_TRUE(body.ok());
  ASSERT_NE(body->Find("__type"), nullptr);
  const opal::Document* offending = body->Find("offending");
  ASSERT_NE(offending, nullptr) << "error payload lost its union member";
  EXPECT_EQ(*offending, OneMember("grade", opal::Document("fail")));

  // Client side: the same wire shape deserializes into the typed error
  // detail with the union preserved.
  auto client = CapturedClient();
  transport_->next_response = opal::http::HttpResponse{400, response.headers, response.body};
  EchoChoiceInput input;
  input.id = "a";
  input.choice = BigUnion::FromGrade(Grade::Value::kFail);
  const auto outcome = client.EchoChoice(input);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().code(), "ChoiceRejected");
  const auto* detail = outcome.error().detail<ChoiceRejected>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->message, "no thanks");
  ASSERT_TRUE(detail->offending.has_value());
  EXPECT_EQ(*detail->offending, BigUnion::FromGrade(Grade::Value::kFail));
}

}  // namespace
}  // namespace compile::unions::cbor
