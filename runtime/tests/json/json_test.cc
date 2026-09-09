#include "smithy/json/json.h"

#include <gtest/gtest.h>

namespace opal::json {
namespace {

Document CityDocument() {
  DocumentMap coords;
  coords.emplace("latitude", Document(47.6));
  coords.emplace("longitude", Document(-122.3));
  DocumentMap map;
  map.emplace("name", Document("Seattle"));
  map.emplace("coordinates", Document(std::move(coords)));
  map.emplace("population", Document(750000));
  map.emplace("rainy", Document(true));
  map.emplace("nickname", Document(nullptr));
  return Document(std::move(map));
}

TEST(JsonTest, EncodesDeterministically) {
  EXPECT_EQ(Encode(CityDocument()),
            R"({"coordinates":{"latitude":47.6,"longitude":-122.3},)"
            R"("name":"Seattle","nickname":null,"population":750000,"rainy":true})");
}

TEST(JsonTest, RoundTripsJsonNativeNodes) {
  const Document original = CityDocument();
  const auto decoded = Decode(Encode(original));
  ASSERT_TRUE(decoded.ok()) << decoded.error().message();
  EXPECT_EQ(*decoded, original);
}

TEST(JsonTest, EncodesBlobAsBase64) {
  DocumentMap map;
  map.emplace("payload", Document(Blob::FromString("foobar")));
  EXPECT_EQ(Encode(Document(std::move(map))), R"({"payload":"Zm9vYmFy"})");
}

TEST(JsonTest, EncodesTimestampsPerFormat) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238000);
  EXPECT_EQ(Encode(Document::FromTimestamp(ts, TimestampFormat::kEpochSeconds)), "1398796238");
  EXPECT_EQ(Encode(Document::FromTimestamp(ts, TimestampFormat::kDateTime)),
            R"("2014-04-29T18:30:38Z")");
  EXPECT_EQ(Encode(Document::FromTimestamp(ts, TimestampFormat::kHttpDate)),
            R"("Tue, 29 Apr 2014 18:30:38 GMT")");
  const auto fractional = Timestamp::FromEpochMilliseconds(1398796238500);
  EXPECT_EQ(Encode(Document::FromTimestamp(fractional, TimestampFormat::kEpochSeconds)),
            "1398796238.5");
}

TEST(JsonTest, DecodesNumbers) {
  const auto doc = Decode(R"({"int":-3,"big":9223372036854775807,"float":0.5})");
  ASSERT_TRUE(doc.ok());
  EXPECT_EQ(doc->Find("int")->as_int(), -3);
  EXPECT_EQ(doc->Find("big")->as_int(), 9223372036854775807LL);
  EXPECT_DOUBLE_EQ(doc->Find("float")->as_double(), 0.5);
}

TEST(JsonTest, RejectsUint64Overflow) { EXPECT_FALSE(Decode("18446744073709551615").ok()); }

TEST(JsonTest, RejectsMalformedText) {
  const char* bad[] = {"", "{", "[1,", R"({"a":})", "tru"};
  for (const char* text : bad) {
    EXPECT_FALSE(Decode(text).ok()) << text;
  }
}

// Regression (fuzzer-found): a string carrying invalid UTF-8 — raw bytes can
// reach Encode via @httpLabel segments, headers, or blobs echoed into a
// response — must not throw (nlohmann's strict dump would terminate the
// server). Invalid sequences become U+FFFD and the output stays valid JSON
// that round-trips.
TEST(JsonTest, EncodeReplacesInvalidUtf8InsteadOfThrowing) {
  for (const std::string& raw : {
           std::string("\xe1"),        // lone 3-byte lead
           std::string("\xff\xfe"),    // never-valid bytes
           std::string("a\x80\x80z"),  // stray continuation bytes
           std::string("ok\xc3"),      // truncated 2-byte sequence
       }) {
    const std::string encoded = Encode(Document(raw));
    const auto reparsed = Decode(encoded);
    ASSERT_TRUE(reparsed.ok()) << "did not round-trip: " << encoded;
    EXPECT_TRUE(reparsed->is_string());
  }
  // Valid UTF-8 (including multibyte) passes through untouched.
  EXPECT_EQ(Encode(Document(std::string("caf\xc3\xa9"))), "\"caf\xc3\xa9\"");
}

// Security regression: every generated JSON server calls Decode() on the
// untrusted request body. Deeply nested input would overflow the stack in
// nlohmann's recursive-descent parser and crash the process; the depth guard
// must reject it as an ordinary error instead. ~100k levels is far past any
// legitimate document and reliably overflowed before the fix.
TEST(JsonTest, RejectsDeeplyNestedInputInsteadOfStackOverflow) {
  for (const char open : {'[', '{'}) {
    std::string bomb(100000, open);
    // Well-formedness is irrelevant — the guard runs before the parser, so an
    // unbalanced bomb is rejected for depth, not for being truncated.
    const auto decoded = Decode(bomb);
    EXPECT_FALSE(decoded.ok());
  }
  // Brackets inside strings don't nest, so a flat document with bracket-heavy
  // string content stays acceptable.
  std::string wide = "[";
  for (int i = 0; i < 1000; ++i) wide += R"("[[[[[[[[[[",)";
  wide += R"("end"])";
  EXPECT_TRUE(Decode(wide).ok());
}

TEST(JsonTest, DecodesNestedLists) {
  const auto doc = Decode(R"([1,[2,"three"],null])");
  ASSERT_TRUE(doc.ok());
  ASSERT_TRUE(doc->is_list());
  EXPECT_EQ(doc->as_list()[1].as_list()[1].as_string(), "three");
  EXPECT_TRUE(doc->as_list()[2].is_null());
}

}  // namespace
}  // namespace opal::json
