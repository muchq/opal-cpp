// Hand-written malformed-server suite for rpcv2Cbor (issue #48). The
// official Smithy conformance suite carries no httpMalformedRequestTests for
// this protocol, so this pins how the generated RpcV2Protocol server rejects
// hostile requests — protocol preconditions, unparseable CBOR, bad routing —
// before the handler runs. Lives outside generated/ because that tree is a
// golden the codegen CI job regenerates byte-for-byte.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "opal/protocoltests/rpcv2cbor/server.h"
#include "smithy/cbor/cbor.h"
#include "smithy/testing/protocol_test.h"

namespace opal::protocoltests::rpcv2cbor {
namespace {

class RecordingHandler : public RpcV2ProtocolHandler {
 public:
  opal::Outcome<EmptyInputOutputOutput> EmptyInputOutput(
      const EmptyInputOutputInput&, const opal::server::RequestContext&) override {
    ++calls;
    return EmptyInputOutputOutput{};
  }
  opal::Outcome<Float16Output> Float16(const Float16Input&,
                                       const opal::server::RequestContext&) override {
    ++calls;
    return Float16Output{};
  }
  opal::Outcome<FractionalSecondsOutput> FractionalSeconds(
      const FractionalSecondsInput&, const opal::server::RequestContext&) override {
    ++calls;
    return FractionalSecondsOutput{};
  }
  opal::Outcome<GreetingWithErrorsOutput> GreetingWithErrors(
      const GreetingWithErrorsInput&, const opal::server::RequestContext&) override {
    ++calls;
    return GreetingWithErrorsOutput{};
  }
  opal::Outcome<NoInputOutputOutput> NoInputOutput(const NoInputOutputInput&,
                                                   const opal::server::RequestContext&) override {
    ++calls;
    return NoInputOutputOutput{};
  }
  opal::Outcome<OperationWithDefaultsOutput> OperationWithDefaults(
      const OperationWithDefaultsInput&, const opal::server::RequestContext&) override {
    ++calls;
    return OperationWithDefaultsOutput{};
  }
  opal::Outcome<OptionalInputOutputOutput> OptionalInputOutput(
      const OptionalInputOutputInput&, const opal::server::RequestContext&) override {
    ++calls;
    return OptionalInputOutputOutput{};
  }
  opal::Outcome<RecursiveShapesOutput> RecursiveShapes(
      const RecursiveShapesInput&, const opal::server::RequestContext&) override {
    ++calls;
    return RecursiveShapesOutput{};
  }
  opal::Outcome<RpcV2CborDenseMapsOutput> RpcV2CborDenseMaps(
      const RpcV2CborDenseMapsInput&, const opal::server::RequestContext&) override {
    ++calls;
    return RpcV2CborDenseMapsOutput{};
  }
  opal::Outcome<RpcV2CborListsOutput> RpcV2CborLists(const RpcV2CborListsInput&,
                                                     const opal::server::RequestContext&) override {
    ++calls;
    return RpcV2CborListsOutput{};
  }
  opal::Outcome<RpcV2CborSparseMapsOutput> RpcV2CborSparseMaps(
      const RpcV2CborSparseMapsInput&, const opal::server::RequestContext&) override {
    ++calls;
    return RpcV2CborSparseMapsOutput{};
  }
  opal::Outcome<RpcV2CborUnionsOutput> RpcV2CborUnions(
      const RpcV2CborUnionsInput&, const opal::server::RequestContext&) override {
    ++calls;
    return RpcV2CborUnionsOutput{};
  }
  opal::Outcome<SimpleScalarPropertiesOutput> SimpleScalarProperties(
      const SimpleScalarPropertiesInput&, const opal::server::RequestContext&) override {
    ++calls;
    return SimpleScalarPropertiesOutput{};
  }
  opal::Outcome<SparseNullsOperationOutput> SparseNullsOperation(
      const SparseNullsOperationInput&, const opal::server::RequestContext&) override {
    ++calls;
    return SparseNullsOperationOutput{};
  }
  int calls = 0;
};

// ::testing, not testing: inside namespace opal::*, protocol_test.h's
// opal::testing shadows gtest's global namespace for unqualified lookup.
class RpcV2CborMalformedTest : public ::testing::Test {
 protected:
  opal::http::HttpRequest WellFormedRequest(const std::string& operation) {
    return opal::testing::Rpcv2CborRequest("RpcV2Protocol", operation);
  }

  opal::http::HttpResponse Send(const opal::http::HttpRequest& request) {
    return server_.Handler()(request);
  }

  // The protocol serializes errors as a CBOR map carrying __type.
  std::string ErrorTypeOf(const opal::http::HttpResponse& response) {
    EXPECT_EQ(response.headers.Get("smithy-protocol").value_or("<missing>"), "rpc-v2-cbor");
    EXPECT_EQ(response.headers.Get("content-type").value_or("<missing>"), "application/cbor");
    const auto body = opal::cbor::Decode(Blob::FromString(response.body));
    EXPECT_TRUE(body.ok());
    if (!body.ok() || !body->is_map()) return "<unparseable>";
    const opal::Document* type = body->Find("__type");
    return type == nullptr ? "<missing>" : std::string(type->as_string());
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RpcV2ProtocolServer server_{handler_};
};

TEST_F(RpcV2CborMalformedTest, MissingSmithyProtocolHeaderIsRejected) {
  auto request = WellFormedRequest("NoInputOutput");
  request.headers.Remove("smithy-protocol");
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, WrongSmithyProtocolHeaderIsRejected) {
  auto request = WellFormedRequest("NoInputOutput");
  request.headers.Set("smithy-protocol", "rpc-v2-json");
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, WrongContentTypeIs415) {
  auto request = WellFormedRequest("SimpleScalarProperties");
  request.headers.Set("content-type", "application/json");
  request.body = "{}";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 415);
  EXPECT_EQ(ErrorTypeOf(response), "UnsupportedMediaTypeException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, TruncatedCborBodyIsSerializationException) {
  auto request = WellFormedRequest("SimpleScalarProperties");
  request.body = "\x18";  // uint8 header, argument byte missing
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, NonMapCborBodyIsSerializationException) {
  auto request = WellFormedRequest("SimpleScalarProperties");
  request.body = "\x01";  // a bare integer where a structure map is required
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, UnknownOperationIs404) {
  const auto response = Send(WellFormedRequest("NoSuchOperation"));
  EXPECT_EQ(response.status, 404);
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, WrongMethodIs405) {
  auto request = WellFormedRequest("NoInputOutput");
  request.method = "GET";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 405);
  EXPECT_EQ(handler_->calls, 0);
}

}  // namespace
}  // namespace opal::protocoltests::rpcv2cbor
