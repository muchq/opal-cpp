// Wire-level tests for the generated rpcv2Cbor cafe client using a capturing
// mock transport: asserts the exact request shape the protocol mandates and
// exercises response/error/tolerance paths. These double as hand-rolled
// protocol tests until the official-suite generator lands (Phase 3b).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "example/cafe/client.h"
#include "example/cafe/serde.h"
#include "opal/cbor/cbor.h"
#include "opal/compression/gzip.h"
#include "opal/http/transport.h"

namespace example::cafe {
namespace {

using opal::Blob;
using opal::Document;
using opal::DocumentMap;

class CapturingTransport : public opal::http::HttpClient {
 public:
  opal::Outcome<opal::http::HttpResponse> Send(const opal::http::HttpRequest& request) override {
    last_request = request;
    return next_response;
  }

  opal::http::HttpRequest last_request;
  opal::http::HttpResponse next_response{200, {}, ""};
};

class CafeClientTest : public testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_shared<CapturingTransport>();
    opal::ClientConfig config;
    config.http_client = transport_;
    auto client = CafeClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<CafeClient>(std::move(*client));
  }

  static std::string EncodeBody(Document doc) { return opal::cbor::Encode(doc).ToString(); }

  static opal::Outcome<Document> DecodeBody(const std::string& body) {
    return opal::cbor::Decode(Blob::FromString(body));
  }

  std::shared_ptr<CapturingTransport> transport_;
  std::unique_ptr<CafeClient> client_;
};

TEST_F(CafeClientTest, RequestMatchesRpcv2CborSpec) {
  DocumentMap output;
  output.emplace("orderId", Document("o-1"));
  output.emplace("status", [] {
    DocumentMap pending;
    pending.emplace("position", Document(2));
    DocumentMap status;
    status.emplace("pending", Document(std::move(pending)));
    return Document(std::move(status));
  }());
  transport_->next_response.body = EncodeBody(Document(std::move(output)));

  OrderCoffeeInput input;
  input.coffeeType = CoffeeType(CoffeeType::Value::kCortado);
  input.milk = MilkOption::FromDairy(DairyMilk{.percentFat = 2.0F});
  const auto result = client_->OrderCoffee(input);
  ASSERT_TRUE(result.ok()) << result.error().message();

  const auto& request = transport_->last_request;
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.target, "/service/Cafe/operation/OrderCoffee");
  EXPECT_EQ(request.headers.Get("smithy-protocol"), "rpc-v2-cbor");
  EXPECT_EQ(request.headers.Get("content-type"), "application/cbor");
  EXPECT_EQ(request.headers.Get("accept"), "application/cbor");

  const auto body = DecodeBody(request.body);
  ASSERT_TRUE(body.ok());
  EXPECT_EQ(body->Find("coffeeType")->as_string(), "CORTADO");
  const Document* milk = body->Find("milk");
  ASSERT_NE(milk, nullptr);
  EXPECT_DOUBLE_EQ(milk->Find("dairy")->Find("percentFat")->AsNumber(), 2.0);
}

TEST_F(CafeClientTest, IdempotencyTokenAutoFills) {
  transport_->next_response.body = EncodeBody([] {
    DocumentMap output;
    output.emplace("orderId", Document("o-2"));
    DocumentMap ready;
    ready.emplace("readyAt", Document::FromTimestamp(opal::Timestamp::FromEpochMilliseconds(5500),
                                                     opal::TimestampFormat::kEpochSeconds));
    DocumentMap status;
    status.emplace("ready", Document(std::move(ready)));
    output.emplace("status", Document(std::move(status)));
    return Document(std::move(output));
  }());

  OrderCoffeeInput input;
  input.coffeeType = CoffeeType(CoffeeType::Value::kDrip);
  const auto result = client_->OrderCoffee(input);  // clientToken unset
  ASSERT_TRUE(result.ok()) << result.error().message();
  ASSERT_TRUE(result->status.is_ready());
  EXPECT_EQ(result->status.as_ready().readyAt.epoch_milliseconds(), 5500);

  const auto body = DecodeBody(transport_->last_request.body);
  ASSERT_TRUE(body.ok());
  const Document* token = body->Find("clientToken");
  ASSERT_NE(token, nullptr) << "unset @idempotencyToken member must be auto-filled";
  EXPECT_EQ(token->as_string().size(), 36u);

  // A caller-provided token is passed through untouched.
  input.clientToken = "caller-token";
  ASSERT_TRUE(client_->OrderCoffee(input).ok());
  const auto second = DecodeBody(transport_->last_request.body);
  EXPECT_EQ(second->Find("clientToken")->as_string(), "caller-token");
}

TEST_F(CafeClientTest, ModeledErrorsDeserialize) {
  DocumentMap error;
  error.emplace("__type", Document("example.cafe#OrderNotFound"));
  error.emplace("message", Document("no such order"));
  error.emplace("orderId", Document("missing"));
  transport_->next_response = {404, {}, EncodeBody(Document(std::move(error)))};

  const auto result = client_->GetOrder(GetOrderInput{.orderId = "missing"});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(result.error().code(), "OrderNotFound");
  EXPECT_EQ(result.error().message(), "no such order");
  EXPECT_FALSE(result.error().retryable());

  // The deserialized error structure rides along as the typed detail.
  const auto* detail = result.error().detail<OrderNotFound>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->orderId, "missing");
  EXPECT_EQ(result.error().detail<OutOfBeans>(), nullptr);
}

TEST_F(CafeClientTest, ServerErrorsAreRetryable) {
  DocumentMap error;
  error.emplace("__type", Document("example.cafe#OutOfBeans"));
  transport_->next_response = {500, {}, EncodeBody(Document(std::move(error)))};
  // OrderCoffee declares OutOfBeans, so the client attaches the typed detail.
  const auto result =
      client_->OrderCoffee(OrderCoffeeInput{.coffeeType = CoffeeType(CoffeeType::Value::kDrip)});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), "OutOfBeans");
  EXPECT_TRUE(result.error().retryable());
  EXPECT_NE(result.error().detail<OutOfBeans>(), nullptr);
}

TEST_F(CafeClientTest, UndeclaredErrorsSurfaceWithoutDetail) {
  DocumentMap error;
  error.emplace("__type", Document("example.cafe#OutOfBeans"));
  transport_->next_response = {500, {}, EncodeBody(Document(std::move(error)))};
  // GetOrder does not declare OutOfBeans: the code still surfaces, but no
  // typed detail is attached for errors outside the operation's contract.
  const auto result = client_->GetOrder(GetOrderInput{.orderId = "o-1"});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), "OutOfBeans");
  EXPECT_TRUE(result.error().retryable());
  EXPECT_FALSE(result.error().has_detail());
}

TEST_F(CafeClientTest, UnknownResponseMembersAreIgnored) {
  DocumentMap output;
  output.emplace("orderId", Document("o-3"));
  output.emplace("coffeeType", Document("LATTE"));
  DocumentMap cancelled;
  cancelled.emplace("reason", Document("closed"));
  DocumentMap status;
  status.emplace("cancelled", Document(std::move(cancelled)));
  output.emplace("status", Document(std::move(status)));
  output.emplace("aFutureMember", Document("new servers may add fields"));
  transport_->next_response.body = EncodeBody(Document(std::move(output)));

  const auto result = client_->GetOrder(GetOrderInput{.orderId = "o-3"});
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(result->coffeeType.value(), CoffeeType::Value::kLatte);
  ASSERT_TRUE(result->status.is_cancelled());
  EXPECT_EQ(result->status.as_cancelled().reason.value(), "closed");
}

TEST_F(CafeClientTest, UnknownEnumValuesArePreserved) {
  DocumentMap output;
  output.emplace("orderId", Document("o-4"));
  output.emplace("coffeeType", Document("OAT_FOAM"));  // future enum value
  DocumentMap pending;
  pending.emplace("position", Document(1));
  DocumentMap status;
  status.emplace("pending", Document(std::move(pending)));
  output.emplace("status", Document(std::move(status)));
  transport_->next_response.body = EncodeBody(Document(std::move(output)));

  const auto result = client_->GetOrder(GetOrderInput{.orderId = "o-4"});
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(result->coffeeType.value(), CoffeeType::Value::kUnknown);
  EXPECT_EQ(result->coffeeType.ToString(), "OAT_FOAM");
}

TEST_F(CafeClientTest, MissingRequiredMemberIsASerdeError) {
  DocumentMap output;
  output.emplace("orderId", Document("o-5"));  // status missing
  transport_->next_response.body = EncodeBody(Document(std::move(output)));
  const auto result =
      client_->OrderCoffee(OrderCoffeeInput{.coffeeType = CoffeeType(CoffeeType::Value::kDrip)});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().kind(), opal::ErrorKind::kSerialization);
  EXPECT_NE(result.error().message().find("status"), std::string::npos);
}

TEST_F(CafeClientTest, SerdeRoundTripsThroughGeneratedFunctions) {
  // Serialize -> Deserialize symmetry for the full input struct.
  OrderCoffeeInput input;
  input.coffeeType = CoffeeType(CoffeeType::Value::kEspresso);
  input.milk = MilkOption::FromAlternative(AlternativeMilk{.kind = "oat"});
  input.clientToken = "t-1";
  const auto round = DeserializeOrderCoffeeInput(SerializeOrderCoffeeInput(input));
  ASSERT_TRUE(round.ok()) << round.error().message();
  EXPECT_EQ(*round, input);
}

// @httpApiKeyAuth(name: "x-api-key", in: "header") — the configured key
// rides every request; absent config leaves requests anonymous.
TEST_F(CafeClientTest, ApiKeyHeaderComesFromConfig) {
  EXPECT_FALSE(transport_->last_request.headers.Get("x-api-key").has_value());

  auto transport = std::make_shared<CapturingTransport>();
  opal::ClientConfig config;
  config.http_client = transport;
  config.api_key = [] { return std::string("cafe-key"); };
  auto client = CafeClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  transport->next_response.body = EncodeBody(Document(DocumentMap{}));
  transport->next_response.headers.Set("smithy-protocol", "rpc-v2-cbor");
  (void)client->GetOrder(GetOrderInput{.orderId = "abc"});
  EXPECT_EQ(transport->last_request.headers.Get("x-api-key"), "cafe-key");
}

// @requestCompression(encodings: ["gzip"]) on OrderCoffee: bodies at or
// above the configured threshold are gzipped (Content-Encoding set, payload
// decompresses back to the CBOR that would otherwise have been sent);
// smaller bodies go out untouched.
TEST_F(CafeClientTest, SmallOrderBodiesAreNotCompressed) {
  transport_->next_response.headers.Set("smithy-protocol", "rpc-v2-cbor");
  transport_->next_response.body = EncodeBody(Document(DocumentMap{}));
  (void)client_->OrderCoffee(OrderCoffeeInput{.coffeeType = CoffeeType::FromString("LATTE")});
  EXPECT_FALSE(transport_->last_request.headers.Get("content-encoding").has_value());
  EXPECT_TRUE(DecodeBody(transport_->last_request.body).ok());
}

TEST_F(CafeClientTest, ThresholdZeroCompressesEverything) {
  auto transport = std::make_shared<CapturingTransport>();
  opal::ClientConfig config;
  config.http_client = transport;
  config.request_min_compression_size_bytes = 0;
  auto client = CafeClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());
  transport->next_response.headers.Set("smithy-protocol", "rpc-v2-cbor");
  transport->next_response.body = EncodeBody(Document(DocumentMap{}));

  (void)client->OrderCoffee(OrderCoffeeInput{.coffeeType = CoffeeType::FromString("LATTE")});
  EXPECT_EQ(transport->last_request.headers.Get("content-encoding"), "gzip");
  const auto decompressed = opal::GzipDecompress(transport->last_request.body);
  ASSERT_TRUE(decompressed.ok()) << decompressed.error().message();
  const auto doc = DecodeBody(*decompressed);
  ASSERT_TRUE(doc.ok());
  EXPECT_TRUE(doc->as_map().contains("coffeeType"));
}

TEST_F(CafeClientTest, LargeOrderBodiesCompressAtTheDefaultThreshold) {
  // clientToken is an unconstrained string: pad the body past 10240 bytes.
  OrderCoffeeInput input{.coffeeType = CoffeeType::FromString("LATTE"),
                         .clientToken = std::string(16 * 1024, 't')};
  transport_->next_response.headers.Set("smithy-protocol", "rpc-v2-cbor");
  transport_->next_response.body = EncodeBody(Document(DocumentMap{}));
  (void)client_->OrderCoffee(input);
  EXPECT_EQ(transport_->last_request.headers.Get("content-encoding"), "gzip");
  const auto decompressed = opal::GzipDecompress(transport_->last_request.body);
  ASSERT_TRUE(decompressed.ok());
  const auto doc = DecodeBody(*decompressed);
  ASSERT_TRUE(doc.ok());
  EXPECT_EQ(doc->as_map().at("clientToken").as_string(), input.clientToken.value());
}

}  // namespace
}  // namespace example::cafe
