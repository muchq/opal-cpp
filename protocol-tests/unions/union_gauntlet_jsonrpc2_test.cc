// Union member-type gauntlet x jsonRpc2 (issue #56): the jsonRpc2 twin of
// union_gauntlet_cbor_test.cc. Same four-direction cells per member kind,
// with the JSON-specific wire representations pinned (blobs as base64 text,
// timestamps as epoch-seconds numbers), a byte-exact request envelope, and
// the error-shape union cell riding the JSON-RPC error object's data member
// next to its __type discriminator.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compile/unions/jsonrpc/client.h"
#include "compile/unions/jsonrpc/server.h"
#include "opal/client/config.h"
#include "opal/core/document.h"
#include "opal/core/timestamp.h"
#include "opal/http/loopback.h"
#include "opal/json/json.h"
#include "opal/testing/protocol_test.h"

namespace compile::unions::jsonrpc {
namespace {

opal::Document Doc(opal::DocumentMap map) { return opal::Document(std::move(map)); }

opal::Document OneMember(const std::string& name, opal::Document value) {
  opal::DocumentMap map;
  map.emplace(name, std::move(value));
  return Doc(std::move(map));
}

// A success envelope answering the client's fixed request id 1.
std::string ResultEnvelope(opal::Document result) {
  opal::DocumentMap envelope;
  envelope.emplace("jsonrpc", opal::Document("2.0"));
  envelope.emplace("id", opal::Document(std::int64_t{1}));
  envelope.emplace("result", std::move(result));
  return opal::json::Encode(Doc(std::move(envelope)));
}

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
  // JSON has no binary type: blobs ride as base64 text on the wire.
  cells.push_back({"data", BigUnion::FromData(opal::Blob::FromString("\x01\x02")),
                   OneMember("data", opal::Document("AQI="))});
  // Timestamps default to epoch-seconds numbers.
  cells.push_back({"when", BigUnion::FromWhen(timestamp),
                   OneMember("when", opal::Document(std::int64_t{1515531081}))});
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

class UnionGauntletJsonRpc2Test : public testing::Test {
 protected:
  void SetUp() override {
    handler_ = std::make_shared<EchoHandler>();
    server_ = std::make_unique<UnionGauntletServer>(handler_);
    transport_ = std::make_shared<opal::testing::CapturingTransport>();
  }

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

TEST_F(UnionGauntletJsonRpc2Test, EveryMemberKindEncodesToItsWireSubdocument) {
  auto client = CapturedClient();
  transport_->next_response =
      opal::http::HttpResponse{200, {}, ResultEnvelope(OneMember("id", opal::Document("a")))};
  for (const Cell& cell : MemberCells()) {
    EchoChoiceInput input;
    input.id = "a";
    input.choice = cell.typed;
    ASSERT_TRUE(client.EchoChoice(input).ok()) << cell.name;
    auto body = opal::json::Decode(transport_->last_request.body);
    ASSERT_TRUE(body.ok()) << cell.name;
    const opal::Document* params = body->Find("params");
    ASSERT_NE(params, nullptr) << cell.name;
    const opal::Document* choice = params->Find("choice");
    ASSERT_NE(choice, nullptr) << cell.name;
    ASSERT_TRUE(choice->is_map()) << cell.name;
    EXPECT_EQ(choice->as_map().size(), 1u) << cell.name;
    EXPECT_EQ(*choice, cell.wire) << cell.name;
  }
}

TEST_F(UnionGauntletJsonRpc2Test, EveryMemberKindSurvivesTheFullClientServerLoop) {
  auto client = LiveClient();
  for (const Cell& cell : MemberCells()) {
    EchoChoiceInput input;
    input.id = "a";
    input.choice = cell.typed;
    const auto outcome = client.EchoChoice(input);
    ASSERT_TRUE(outcome.ok()) << cell.name << ": " << outcome.error().message();
    ASSERT_TRUE(handler_->last.has_value()) << cell.name;
    ASSERT_TRUE(handler_->last->choice.has_value()) << cell.name;
    EXPECT_EQ(*handler_->last->choice, cell.typed) << cell.name;
    ASSERT_TRUE(outcome->choice.has_value()) << cell.name;
    EXPECT_EQ(*outcome->choice, cell.typed) << cell.name;
    handler_->last.reset();
  }
}

// The full envelope text, byte for byte: the JSON encoder emits compact
// output with lexicographically sorted keys (DocumentMap is a sorted map),
// so this string is deterministic.
TEST_F(UnionGauntletJsonRpc2Test, RequestBodyIsByteExact) {
  auto client = CapturedClient();
  transport_->next_response =
      opal::http::HttpResponse{200, {}, ResultEnvelope(OneMember("id", opal::Document("a")))};
  EchoChoiceInput input;
  input.id = "a";
  input.choice = BigUnion::FromText("hi");
  ASSERT_TRUE(client.EchoChoice(input).ok());
  EXPECT_EQ(transport_->last_request.body,
            "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"EchoChoice\","
            "\"params\":{\"choice\":{\"text\":\"hi\"},\"id\":\"a\"}}");
}

TEST_F(UnionGauntletJsonRpc2Test, ModeledErrorCarriesAUnionNextToItsTypeDiscriminator) {
  handler_->reject = true;

  // Server side: the error envelope's data member carries __type AND the
  // union member.
  opal::DocumentMap envelope;
  envelope.emplace("jsonrpc", opal::Document("2.0"));
  envelope.emplace("method", opal::Document("EchoChoice"));
  envelope.emplace("id", opal::Document(std::int64_t{7}));
  opal::DocumentMap params;
  params.emplace("id", opal::Document("a"));
  params.emplace("choice", OneMember("grade", opal::Document("fail")));
  envelope.emplace("params", Doc(std::move(params)));
  opal::http::HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.headers.Set("content-type", "application/json");
  request.body = opal::json::Encode(Doc(std::move(envelope)));
  const auto response = server_->Handler()(request);
  EXPECT_EQ(response.status, 200) << response.body;
  auto body = opal::json::Decode(response.body);
  ASSERT_TRUE(body.ok()) << response.body;
  const opal::Document* error = body->Find("error");
  ASSERT_NE(error, nullptr) << response.body;
  const opal::Document* data = error->Find("data");
  ASSERT_NE(data, nullptr) << response.body;
  ASSERT_NE(data->Find("__type"), nullptr) << response.body;
  const opal::Document* offending = data->Find("offending");
  ASSERT_NE(offending, nullptr) << "error payload lost its union member: " << response.body;
  EXPECT_EQ(*offending, OneMember("grade", opal::Document("fail")));

  // Client side: the same envelope deserializes into the typed error detail
  // with the union preserved.
  auto client = CapturedClient();
  transport_->next_response = opal::http::HttpResponse{response.status, {}, response.body};
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
}  // namespace compile::unions::jsonrpc
