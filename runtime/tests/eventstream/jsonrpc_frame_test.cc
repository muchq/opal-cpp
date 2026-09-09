// Pins ADR-0023's JSON-RPC stream wire: the exact notification text the
// encoder mints (byte-pinned — text frames ARE the wire), the round trip
// back to the Message the binary wire would carry, the terminal-response
// classification (result = the clean end, error = the exception message),
// and the fail-closed bank — every malformed envelope must classify as a
// kViolation carrying its reserved code, never as a half-understood
// message.

#include "opal/eventstream/jsonrpc_frame.h"

#include <gtest/gtest.h>

#include <string>

#include "opal/core/document.h"
#include "opal/eventstream/envelope.h"
#include "opal/eventstream/frame.h"

namespace opal::eventstream {
namespace {

const Document kId(1);

std::string EncodeOrDie(const Message& message, const Document& id = kId) {
  auto text = EncodeJsonRpcNotification(message, id);
  EXPECT_TRUE(text.ok()) << text.error().message();
  return text.ok() ? *text : std::string();
}

JsonRpcStreamFrame DecodeOrDie(std::string_view text, const Document& id = kId) {
  JsonRpcStreamFrame frame = DecodeJsonRpcStreamFrame(text, id);
  EXPECT_NE(frame.kind, JsonRpcStreamFrame::Kind::kViolation) << frame.reason;
  return frame;
}

void ExpectEncodeRefusal(const Message& message, const char* why) {
  const auto text = EncodeJsonRpcNotification(message, kId);
  ASSERT_FALSE(text.ok()) << why;
  EXPECT_EQ(text.error().kind(), ErrorKind::kValidation) << why;
}

// A grammar failure classifies as kViolation carrying its reserved code —
// -32700 for unparseable text, -32600 for everything else (ADR-0023). The
// codes are wire truth (the server answers them), so every case pins its
// code, not just the refusal.
void ExpectViolation(std::string_view text, int code, const char* why, const Document& id = kId) {
  const JsonRpcStreamFrame frame = DecodeJsonRpcStreamFrame(text, id);
  ASSERT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kViolation) << why;
  EXPECT_EQ(frame.code, code) << why << " — reason: " << frame.reason;
  EXPECT_FALSE(frame.reason.empty()) << why;
}

TEST(JsonRpcFrameTest, EventsRenderThePinnedNotificationText) {
  // Byte-pinned: this text is what a browser's onmessage receives. The
  // codec's JSON output is deterministic (sorted keys, compact), so
  // string equality is wire equality. The id echo rides inside params —
  // the eth_subscribe shape (ADR-0023).
  const std::string text = EncodeOrDie(
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})")));
  EXPECT_EQ(text,
            R"({"jsonrpc":"2.0","method":"message","params":{"id":1,"payload":{"text":"hi"}}})");
}

TEST(JsonRpcFrameTest, AnAbsentContentTypeEncodesLikeJson) {
  const std::string text = EncodeOrDie(MakeEventMessage("ping", "", Blob::FromString("{}")));
  EXPECT_EQ(text, R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}}})");
}

TEST(JsonRpcFrameTest, NotificationsDecodeBackToTheBinaryWireMessage) {
  const Message original =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})"));
  const JsonRpcStreamFrame frame = DecodeOrDie(EncodeOrDie(original));
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kEvent);
  EXPECT_EQ(frame.message, original);
}

TEST(JsonRpcFrameTest, DecodeIsInsensitiveToMemberOrderAndWhitespace) {
  // What a hand-written browser client actually sends: JSON.stringify's
  // insertion order, or pretty-printed text. Same Message either way.
  const JsonRpcStreamFrame compact = DecodeOrDie(
      R"({"jsonrpc":"2.0","method":"message","params":{"id":1,"payload":{"text":"hi"}}})");
  const JsonRpcStreamFrame reordered = DecodeOrDie(
      " { \"params\" : { \"payload\" : { \"text\" : \"hi\" } , \"id\" : 1 } ,"
      " \"method\" : \"message\" , \"jsonrpc\" : \"2.0\" } ");
  EXPECT_EQ(compact.message, reordered.message);
}

TEST(JsonRpcFrameTest, NonIntegerIdsEchoAndMatch) {
  // The opening id is whatever the opening call carried — matched by
  // value, echoed verbatim. Type matters: 1 and "1" are different calls.
  const Document string_id("abc-1");
  const std::string text =
      EncodeOrDie(MakeEventMessage("ping", "", Blob::FromString("{}")), string_id);
  EXPECT_EQ(text, R"({"jsonrpc":"2.0","method":"ping","params":{"id":"abc-1","payload":{}}})");
  EXPECT_EQ(DecodeOrDie(text, string_id).kind, JsonRpcStreamFrame::Kind::kEvent);
  ExpectViolation(text, -32600, "string echo against integer id");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":"1","payload":{}}})", -32600,
                  "\"1\" is not 1");
}

TEST(JsonRpcFrameTest, ExceptionsCannotRideNotifications) {
  // Terminal envelopes are the generated serve path's business — it owns
  // the @httpError table; the wrapper never guesses a code (ADR-0023).
  ExpectEncodeRefusal(
      MakeExceptionMessage("Kicked", "application/json", Blob::FromString(R"({"by":"mod"})")),
      "exception message");
}

TEST(JsonRpcFrameTest, HeadersBeyondTheEnvelopeCannotRideTheWire) {
  // The notification has no header channel; encode refuses what the wire
  // cannot represent rather than dropping it (ADR-0014's rule).
  Message extra = MakeEventMessage("ping", "application/json", Blob::FromString("{}"));
  extra.headers.push_back({"x-app-extra", std::string("boom")});
  ExpectEncodeRefusal(extra, "extra header");

  ExpectEncodeRefusal(
      Message{.headers = {{":event-type", "chat"}}, .payload = Blob::FromString("{}")},
      "no :message-type");
}

TEST(JsonRpcFrameTest, NonJsonPayloadsAndContentTypesAreRefused) {
  ExpectEncodeRefusal(MakeEventMessage("ping", "application/cbor", Blob::FromString(R"({"n":1})")),
                      "cbor content type");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("[1,2]")), "array payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob()), "empty payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("{not json")),
                      "malformed payload");
}

TEST(JsonRpcFrameTest, TheTerminalResultClassifiesAsTheCleanEnd) {
  const JsonRpcStreamFrame frame = DecodeOrDie(R"({"jsonrpc":"2.0","result":{},"id":1})");
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kResult);
  EXPECT_EQ(frame.result, Document(DocumentMap()));
  // The value is preserved but not policed — whatever rides here when
  // initial-response support lands is terminal either way.
  const JsonRpcStreamFrame carried =
      DecodeOrDie(R"({"jsonrpc":"2.0","result":{"note":"bye"},"id":1})");
  ASSERT_TRUE(carried.result.is_map());
  ASSERT_NE(carried.result.Find("note"), nullptr);
}

TEST(JsonRpcFrameTest, TheTerminalErrorClassifiesAsTheExceptionMessage) {
  // The unary error-object convention, unchanged (ADR-0023): data.__type
  // names the exception, data is the modeled payload.
  const JsonRpcStreamFrame frame =
      DecodeOrDie(R"({"jsonrpc":"2.0","error":{"code":409,"message":"kicked",)"
                  R"("data":{"__type":"example.chat#Kicked","by":"mod"}},"id":1})");
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kException);
  ASSERT_NE(frame.message.FindString(":exception-type"), nullptr);
  EXPECT_EQ(*frame.message.FindString(":exception-type"), "example.chat#Kicked");
  // The error message fills a data object that carries none — the unary
  // client's fallback, mirrored — and __type rides along harmlessly.
  EXPECT_EQ(frame.message.payload.ToString(),
            R"({"__type":"example.chat#Kicked","by":"mod","message":"kicked"})");
}

TEST(JsonRpcFrameTest, ADataMessageMemberIsNotOverwritten) {
  const JsonRpcStreamFrame frame =
      DecodeOrDie(R"({"jsonrpc":"2.0","error":{"code":400,"message":"outer",)"
                  R"("data":{"__type":"X","message":"modeled"}},"id":1})");
  EXPECT_EQ(frame.message.payload.ToString(), R"({"__type":"X","message":"modeled"})");
}

TEST(JsonRpcFrameTest, AnUntypedErrorFallsBackToTheGenericException) {
  // A foreign server may omit data or __type; the fallback type matches no
  // modeled shape, so generated decoders surface the generic terminal
  // error instead of misclassifying.
  const JsonRpcStreamFrame frame =
      DecodeOrDie(R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"unknown method"},"id":1})");
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kException);
  ASSERT_NE(frame.message.FindString(":exception-type"), nullptr);
  EXPECT_EQ(*frame.message.FindString(":exception-type"), "JsonRpcError");
  EXPECT_EQ(frame.message.payload.ToString(), R"({"message":"unknown method"})");
}

TEST(JsonRpcFrameTest, ResponsesAndEchoesForAForeignIdAreRefused) {
  // One stream per socket: every frame belongs to the opening call.
  ExpectViolation(R"({"jsonrpc":"2.0","result":{},"id":2})", -32600, "foreign response id");
  ExpectViolation(R"({"jsonrpc":"2.0","result":{}})", -32600, "missing response id");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":2,"payload":{}}})", -32600,
                  "foreign notification id");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"payload":{}}})", -32600,
                  "missing notification id echo");
}

TEST(JsonRpcFrameTest, ARequestEnvelopeAfterTheOpeningCallIsRefused) {
  // A method member plus a top-level id is a second call — there is no
  // second call on a one-stream socket (multiplexing is the door the id
  // echo leaves open, not a thing this wire speaks).
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{},"id":1})", -32600,
                  "request envelope");
}

TEST(JsonRpcFrameTest, TheFailClosedBankRefusesEveryMalformedEnvelope) {
  ExpectViolation("not json at all", -32700, "not JSON");
  ExpectViolation("", -32700, "empty text");
  ExpectViolation("null", -32600, "null envelope");
  ExpectViolation("[]", -32600, "array envelope");
  ExpectViolation("{}", -32600, "empty envelope");
  ExpectViolation(R"({"method":"ping","params":{"id":1,"payload":{}}})", -32600, "missing jsonrpc");
  ExpectViolation(R"({"jsonrpc":"1.0","method":"ping","params":{"id":1,"payload":{}}})", -32600,
                  "wrong version");
  ExpectViolation(R"({"jsonrpc":2,"method":"ping","params":{"id":1,"payload":{}}})", -32600,
                  "non-string version");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}},"x":1})",
                  -32600, "unknown member");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}},)"
                  R"("result":{}})",
                  -32600, "notification mixed with response");
  ExpectViolation(R"({"jsonrpc":"2.0","id":1})", -32600, "neither notification nor response");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"","params":{"id":1,"payload":{}}})", -32600,
                  "empty method");
  ExpectViolation(R"({"jsonrpc":"2.0","method":5,"params":{"id":1,"payload":{}}})", -32600,
                  "non-string method");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping"})", -32600, "missing params");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":[1]})", -32600, "array params");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{},"x":1}})",
                  -32600, "unknown params member");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1}})", -32600,
                  "missing payload");
  ExpectViolation(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":[1]}})", -32600,
                  "array payload");
  ExpectViolation(R"({"jsonrpc":"2.0","result":{},"error":{"code":1},"id":1})", -32600,
                  "both result and error");
  ExpectViolation(R"({"jsonrpc":"2.0","error":[1],"id":1})", -32600, "array error");
  ExpectViolation(R"({"jsonrpc":"2.0","error":{},"id":1})", -32600, "missing code");
  ExpectViolation(R"({"jsonrpc":"2.0","error":{"code":"x"},"id":1})", -32600, "non-integer code");
  ExpectViolation(R"({"jsonrpc":"2.0","error":{"code":1,"message":5},"id":1})", -32600,
                  "non-string message");
  ExpectViolation(R"({"jsonrpc":"2.0","error":{"code":1,"data":[1]},"id":1})", -32600,
                  "non-object data");
  ExpectViolation(R"({"jsonrpc":"2.0","error":{"code":1,"extra":true},"id":1})", -32600,
                  "unknown error member");
}

TEST(JsonRpcFrameTest, TheSizeCeilingHoldsInBothDirections) {
  const std::string huge_payload = R"({"blob":")" + std::string(kMaxMessageBytes, 'a') + R"("})";
  ExpectEncodeRefusal(MakeEventMessage("big", "", Blob::FromString(huge_payload)),
                      "oversized encode");
  const std::string huge_frame =
      R"({"jsonrpc":"2.0","method":"big","params":{"id":1,"payload":)" + huge_payload + "}}";
  ExpectViolation(huge_frame, -32600, "oversized decode");
}

TEST(JsonRpcFrameTest, TheViolationResponseRendersThePinnedRefusalShape) {
  // Byte-pinned: the terminal error a server answers an envelope-level
  // violation with — the unary refusal shape exactly.
  EXPECT_EQ(EncodeJsonRpcViolationResponse(-32700, "text frame is not JSON", Document(42)),
            R"({"error":{"code":-32700,"data":{"__type":"SerializationException"},)"
            R"("message":"text frame is not JSON"},"id":42,"jsonrpc":"2.0"})");
  EXPECT_EQ(EncodeJsonRpcViolationResponse(-32600, "why", Document()),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"why"},"id":null,"jsonrpc":"2.0"})");
}

}  // namespace
}  // namespace opal::eventstream
