#include <gtest/gtest.h>

#include <compare>
#include <string>
#include <utility>

#include "smithy/core/base64.h"
#include "smithy/core/blob.h"
#include "smithy/core/document.h"
#include "smithy/core/error.h"
#include "smithy/core/outcome.h"
#include "smithy/core/text.h"

namespace opal {
namespace {

TEST(OutcomeTest, HoldsValue) {
  Outcome<int> outcome(42);
  ASSERT_TRUE(outcome.ok());
  EXPECT_TRUE(static_cast<bool>(outcome));
  EXPECT_EQ(*outcome, 42);
  EXPECT_EQ(outcome.value_or(7), 42);
}

TEST(OutcomeTest, HoldsError) {
  Outcome<int> outcome(Error::Transport("connection refused"));
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);
  EXPECT_EQ(outcome.error().message(), "connection refused");
  EXPECT_TRUE(outcome.error().retryable());
  EXPECT_EQ(outcome.value_or(7), 7);
}

TEST(OutcomeTest, MovesValueOut) {
  Outcome<std::string> outcome(std::string("payload"));
  const std::string moved = std::move(outcome).value();
  EXPECT_EQ(moved, "payload");
}

TEST(OutcomeTest, UnitForVoidLikeOperations) {
  Outcome<Unit> outcome(Unit{});
  EXPECT_TRUE(outcome.ok());
}

// Unit must stay orderable: a union with a Unit member would otherwise
// silently lose its defaulted <=> (the generator emits one because the Unit
// shape itself is orderable).
static_assert(std::three_way_comparable<Unit>);

TEST(OutcomeTest, ValueOrDieReturnsTheValue) {
  Outcome<int> outcome(42);
  EXPECT_EQ(outcome.value_or_die("reading the answer"), 42);
}

// Contract violations die with the error's context instead of throwing a
// context-free std::bad_variant_access (issue #49).

TEST(OutcomeDeathTest, ValueOnErrorDiesWithCodeAndMessage) {
  Outcome<int> outcome(Error::Transport("connection refused"));
  EXPECT_DEATH((void)outcome.value(),
               "Outcome::value\\(\\) on error: TransportError: connection refused");
}

TEST(OutcomeDeathTest, DerefOnErrorDiesWithCodeAndMessage) {
  Outcome<int> outcome(Error::Modeled("OrderNotFound", "no such order"));
  EXPECT_DEATH((void)*outcome, "Outcome::value\\(\\) on error: OrderNotFound: no such order");
}

TEST(OutcomeDeathTest, MovedValueOnErrorDies) {
  EXPECT_DEATH((void)Outcome<std::string>(Error::Validation("bad input")).value(),
               "ValidationError: bad input");
}

TEST(OutcomeDeathTest, ValueOrDieCarriesCallerContext) {
  Outcome<int> outcome(Error::Transport("connection refused"));
  EXPECT_DEATH((void)outcome.value_or_die("creating cafe client"),
               "creating cafe client: TransportError: connection refused");
}

TEST(OutcomeDeathTest, ErrorOnValueDies) {
  Outcome<int> outcome(42);
  EXPECT_DEATH((void)outcome.error(), "Outcome::error\\(\\) called on a value");
}

TEST(OutcomeDeathTest, ErrorTypeWithoutCodeStillDiesWithContext) {
  Outcome<int, std::string> outcome(std::string("boom"));
  EXPECT_DEATH((void)outcome.value(), "Outcome::value\\(\\) on error");
}

TEST(ErrorTest, FactoriesSetKindAndCode) {
  EXPECT_EQ(Error::Serialization("x").kind(), ErrorKind::kSerialization);
  EXPECT_EQ(Error::Validation("x").kind(), ErrorKind::kValidation);
  EXPECT_FALSE(Error::Serialization("x").retryable());
  const Error modeled = Error::Modeled("NoSuchResource", "gone", false);
  EXPECT_EQ(modeled.kind(), ErrorKind::kModeled);
  EXPECT_EQ(modeled.code(), "NoSuchResource");
}

TEST(ErrorTest, DetailCarriesTypedPayload) {
  struct OrderNotFound {
    std::string orderId;
  };
  Error error = Error::Modeled("OrderNotFound", "gone");
  EXPECT_FALSE(error.has_detail());
  EXPECT_EQ(error.detail<OrderNotFound>(), nullptr);

  error.set_detail(OrderNotFound{"o-1"});
  ASSERT_TRUE(error.has_detail());
  ASSERT_NE(error.detail<OrderNotFound>(), nullptr);
  EXPECT_EQ(error.detail<OrderNotFound>()->orderId, "o-1");
  EXPECT_EQ(error.detail<int>(), nullptr) << "wrong type must not cast";

  // Equality is classification-only; detail never participates.
  EXPECT_EQ(error, Error::Modeled("OrderNotFound", "gone"));
}

TEST(TextTest, Utf8CodePointCount) {
  EXPECT_EQ(Utf8CodePointCount(""), 0u);
  EXPECT_EQ(Utf8CodePointCount("abc"), 3u);
  EXPECT_EQ(Utf8CodePointCount("caf\xC3\xA9"), 4u);       // café
  EXPECT_EQ(Utf8CodePointCount("\xF0\x9F\x98\xB9"), 1u);  // one emoji
}

TEST(BlobTest, RoundTripsThroughString) {
  const Blob blob = Blob::FromString("hello");
  EXPECT_EQ(blob.size(), 5u);
  EXPECT_EQ(blob.ToString(), "hello");
  EXPECT_EQ(blob, Blob::FromString("hello"));
  EXPECT_FALSE(blob == Blob::FromString("world"));
}

TEST(BlobTest, OrdersLexicographicallyByBytes) {
  // operator<=> so blob-bearing generated structs stay orderable (issue #49).
  EXPECT_TRUE(Blob::FromString("abc") < Blob::FromString("abd"));
  EXPECT_TRUE(Blob::FromString("ab") < Blob::FromString("abc"));
  EXPECT_TRUE(Blob::FromString("b") > Blob::FromString("abc"));
}

TEST(Base64Test, EncodesRfc4648Vectors) {
  EXPECT_EQ(Base64Encode(Blob::FromString("")), "");
  EXPECT_EQ(Base64Encode(Blob::FromString("f")), "Zg==");
  EXPECT_EQ(Base64Encode(Blob::FromString("fo")), "Zm8=");
  EXPECT_EQ(Base64Encode(Blob::FromString("foo")), "Zm9v");
  EXPECT_EQ(Base64Encode(Blob::FromString("foob")), "Zm9vYg==");
  EXPECT_EQ(Base64Encode(Blob::FromString("fooba")), "Zm9vYmE=");
  EXPECT_EQ(Base64Encode(Blob::FromString("foobar")), "Zm9vYmFy");
}

TEST(Base64Test, DecodesRfc4648Vectors) {
  const std::pair<const char*, const char*> vectors[] = {{"", ""},
                                                         {"Zg==", "f"},
                                                         {"Zm8=", "fo"},
                                                         {"Zm9v", "foo"},
                                                         {"Zm9vYg==", "foob"},
                                                         {"Zm9vYmE=", "fooba"},
                                                         {"Zm9vYmFy", "foobar"}};
  for (const auto& [encoded, expected] : vectors) {
    const auto decoded = Base64Decode(encoded);
    ASSERT_TRUE(decoded.ok()) << encoded;
    EXPECT_EQ(decoded->ToString(), expected);
  }
}

TEST(Base64Test, RoundTripsBinary) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(256);
  for (int i = 0; i < 256; ++i) bytes.push_back(static_cast<std::uint8_t>(i));
  const Blob blob{std::move(bytes)};
  const auto decoded = Base64Decode(Base64Encode(blob));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, blob);
}

TEST(Base64Test, RejectsMalformedInput) {
  const char* bad[] = {"Zg", "Zg=", "====", "Zm9v!A==", "=Zg=", "Z===", "Zh==", "Zm9="};
  for (const char* text : bad) {
    EXPECT_FALSE(Base64Decode(text).ok()) << text;
  }
}

TEST(DocumentTest, DefaultIsNull) {
  const Document doc;
  EXPECT_TRUE(doc.is_null());
}

TEST(DocumentTest, HoldsScalars) {
  EXPECT_TRUE(Document(true).is_bool());
  EXPECT_EQ(Document(7).as_int(), 7);
  EXPECT_DOUBLE_EQ(Document(1.5).as_double(), 1.5);
  EXPECT_EQ(Document("text").as_string(), "text");
  EXPECT_DOUBLE_EQ(Document(7).AsNumber(), 7.0);
  EXPECT_DOUBLE_EQ(Document(1.5).AsNumber(), 1.5);
}

TEST(DocumentTest, HoldsBlobAndTimestamp) {
  const Document blob(Blob::FromString("abc"));
  EXPECT_TRUE(blob.is_blob());
  const Document ts =
      Document::FromTimestamp(Timestamp::FromEpochMilliseconds(1000), TimestampFormat::kDateTime);
  ASSERT_TRUE(ts.is_timestamp());
  EXPECT_EQ(ts.as_timestamp().format, TimestampFormat::kDateTime);
}

TEST(DocumentTest, MapAccessAndEquality) {
  DocumentMap map;
  map.emplace("name", Document("Seattle"));
  map.emplace("population", Document(750000));
  const Document doc(std::move(map));
  ASSERT_TRUE(doc.is_map());
  ASSERT_NE(doc.Find("name"), nullptr);
  EXPECT_EQ(doc.Find("name")->as_string(), "Seattle");
  EXPECT_EQ(doc.Find("missing"), nullptr);
  EXPECT_EQ(Document(7), Document(7));
  EXPECT_FALSE(Document(7) == Document(8));
  EXPECT_FALSE(Document(7) == Document(7.0));  // int and double are distinct nodes
}

TEST(DocumentTest, ListAccess) {
  DocumentList list;
  list.emplace_back(1);
  list.emplace_back("two");
  const Document doc(std::move(list));
  ASSERT_TRUE(doc.is_list());
  ASSERT_EQ(doc.as_list().size(), 2u);
  EXPECT_EQ(doc.as_list()[1].as_string(), "two");
  EXPECT_EQ(doc.Find("key"), nullptr);  // Find on a non-map is safe
}

}  // namespace
}  // namespace opal
