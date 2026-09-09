#include "smithy/cbor/cbor.h"

#include <gtest/gtest.h>

#include <string>

namespace opal::cbor {
namespace {

std::string ToHex(const Blob& blob) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t byte : blob.bytes()) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0xF]);
  }
  return out;
}

Blob FromHex(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(
        static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return Blob(std::move(bytes));
}

// Expected encodings from RFC 8949 Appendix A.
TEST(CborTest, EncodesRfc8949Vectors) {
  EXPECT_EQ(ToHex(Encode(Document(0))), "00");
  EXPECT_EQ(ToHex(Encode(Document(10))), "0a");
  EXPECT_EQ(ToHex(Encode(Document(23))), "17");
  EXPECT_EQ(ToHex(Encode(Document(24))), "1818");
  EXPECT_EQ(ToHex(Encode(Document(100))), "1864");
  EXPECT_EQ(ToHex(Encode(Document(1000))), "1903e8");
  EXPECT_EQ(ToHex(Encode(Document(1000000))), "1a000f4240");
  EXPECT_EQ(ToHex(Encode(Document(std::int64_t{1000000000000}))), "1b000000e8d4a51000");
  EXPECT_EQ(ToHex(Encode(Document(-1))), "20");
  EXPECT_EQ(ToHex(Encode(Document(-10))), "29");
  EXPECT_EQ(ToHex(Encode(Document(-100))), "3863");
  EXPECT_EQ(ToHex(Encode(Document(-1000))), "3903e7");
  EXPECT_EQ(ToHex(Encode(Document(false))), "f4");
  EXPECT_EQ(ToHex(Encode(Document(true))), "f5");
  EXPECT_EQ(ToHex(Encode(Document(nullptr))), "f6");
  EXPECT_EQ(ToHex(Encode(Document(1.1))), "fb3ff199999999999a");
  EXPECT_EQ(ToHex(Encode(Document(""))), "60");
  EXPECT_EQ(ToHex(Encode(Document("IETF"))), "6449455446");
  EXPECT_EQ(ToHex(Encode(Document(Blob::FromString("\x01\x02\x03\x04")))), "4401020304");

  DocumentList list;
  list.emplace_back(1);
  list.emplace_back(2);
  list.emplace_back(3);
  EXPECT_EQ(ToHex(Encode(Document(std::move(list)))), "83010203");

  DocumentMap inner_map;
  DocumentList inner_list;
  inner_list.emplace_back(2);
  inner_list.emplace_back(3);
  inner_map.emplace("a", Document(1));
  inner_map.emplace("b", Document(std::move(inner_list)));
  EXPECT_EQ(ToHex(Encode(Document(std::move(inner_map)))), "a26161016162820203");
}

TEST(CborTest, EncodesTimestampAsTag1) {
  // RFC 8949: 1(1363896240) => c11a514b67b0
  const auto ts = Timestamp::FromEpochMilliseconds(1363896240000);
  EXPECT_EQ(ToHex(Encode(Document::FromTimestamp(ts, TimestampFormat::kEpochSeconds))),
            "c11a514b67b0");
  // Fractional seconds use a double: 1(1363896240.5) => c1fb41d452d9ec200000
  const auto fractional = Timestamp::FromEpochMilliseconds(1363896240500);
  EXPECT_EQ(ToHex(Encode(Document::FromTimestamp(fractional, TimestampFormat::kEpochSeconds))),
            "c1fb41d452d9ec200000");
}

TEST(CborTest, DecodesTag1Timestamps) {
  const auto integral = Decode(FromHex("c11a514b67b0"));
  ASSERT_TRUE(integral.ok());
  ASSERT_TRUE(integral->is_timestamp());
  EXPECT_EQ(integral->as_timestamp().value.epoch_milliseconds(), 1363896240000);

  const auto fractional = Decode(FromHex("c1fb41d452d9ec200000"));
  ASSERT_TRUE(fractional.ok());
  ASSERT_TRUE(fractional->is_timestamp());
  EXPECT_EQ(fractional->as_timestamp().value.epoch_milliseconds(), 1363896240500);
}

// A ~9-byte tag-1 payload whose integer seconds, scaled to milliseconds, would
// overflow int64 (the old `as_int() * 1000` was undefined behavior). Likewise a
// tag-1 double far outside the representable range. Both must be a clean
// decode error, not UB.
TEST(CborTest, RejectsOutOfRangeTag1TimestampsWithoutOverflow) {
  // c1 1b 7fffffffffffffff = tag 1 + uint64 9223372036854775807 seconds.
  EXPECT_FALSE(Decode(FromHex("c11b7fffffffffffffff")).ok());
  // c1 3b 7fffffffffffffff = tag 1 + a huge negative integer.
  EXPECT_FALSE(Decode(FromHex("c13b7fffffffffffffff")).ok());
  // c1 fb 7fe0000000000000 = tag 1 + double ~1.8e308 (near DBL_MAX).
  EXPECT_FALSE(Decode(FromHex("c1fb7fe0000000000000")).ok());
}

TEST(CborTest, RoundTripsComplexDocument) {
  DocumentMap map;
  map.emplace("null", Document(nullptr));
  map.emplace("bool", Document(true));
  map.emplace("int", Document(-42));
  map.emplace("double", Document(0.5));
  map.emplace("text", Document("héllo"));
  map.emplace("blob", Document(Blob(std::vector<std::uint8_t>{0x00, 0xFF, 0x10})));
  DocumentList list;
  list.emplace_back(1);
  list.emplace_back("two");
  map.emplace("list", Document(std::move(list)));
  map.emplace("ts", Document::FromTimestamp(Timestamp::FromEpochMilliseconds(1500),
                                            TimestampFormat::kEpochSeconds));
  const Document original(std::move(map));
  const auto decoded = Decode(Encode(original));
  ASSERT_TRUE(decoded.ok()) << decoded.error().message();
  EXPECT_EQ(*decoded, original);
}

TEST(CborTest, DecodesIndefiniteLengths) {
  // [_ 1, [2, 3], [_ 4, 5]] => 9f018202039f0405ffff
  const auto list = Decode(FromHex("9f018202039f0405ffff"));
  ASSERT_TRUE(list.ok()) << list.error().message();
  ASSERT_TRUE(list->is_list());
  ASSERT_EQ(list->as_list().size(), 3u);
  EXPECT_EQ(list->as_list()[2].as_list()[1].as_int(), 5);

  // {_ "a": 1, "b": [_ 2, 3]} => bf61610161629f0203ffff
  const auto map = Decode(FromHex("bf61610161629f0203ffff"));
  ASSERT_TRUE(map.ok());
  EXPECT_EQ(map->Find("b")->as_list()[1].as_int(), 3);

  // (_ "strea", "ming") => 7f657374726561646d696e67ff
  const auto text = Decode(FromHex("7f657374726561646d696e67ff"));
  ASSERT_TRUE(text.ok());
  EXPECT_EQ(text->as_string(), "streaming");
}

TEST(CborTest, DecodesAllFloatWidths) {
  const auto half = Decode(FromHex("f93c00"));  // 1.0 as float16
  ASSERT_TRUE(half.ok());
  EXPECT_DOUBLE_EQ(half->as_double(), 1.0);
  const auto subnormal_half = Decode(FromHex("f90001"));
  ASSERT_TRUE(subnormal_half.ok());
  EXPECT_DOUBLE_EQ(subnormal_half->as_double(), 5.960464477539063e-8);
  const auto single = Decode(FromHex("fa47c35000"));  // 100000.0 as float32
  ASSERT_TRUE(single.ok());
  EXPECT_DOUBLE_EQ(single->as_double(), 100000.0);
  const auto negative_half = Decode(FromHex("f9c400"));  // -4.0
  ASSERT_TRUE(negative_half.ok());
  EXPECT_DOUBLE_EQ(negative_half->as_double(), -4.0);
}

TEST(CborTest, SkipsUnknownTags) {
  // 32("http://www.example.com") — URI tag; the inner string should survive.
  const auto doc = Decode(FromHex("d82076687474703a2f2f7777772e6578616d706c652e636f6d"));
  ASSERT_TRUE(doc.ok()) << doc.error().message();
  EXPECT_EQ(doc->as_string(), "http://www.example.com");
}

TEST(CborTest, DecodesUndefinedAsNull) {
  const auto doc = Decode(FromHex("f7"));
  ASSERT_TRUE(doc.ok());
  EXPECT_TRUE(doc->is_null());
}

TEST(CborTest, RejectsMalformedInput) {
  const char* bad[] = {
      "",                    // empty
      "18",                  // truncated uint8 argument
      "1b00000000",          // truncated uint64 argument
      "62e6",                // truncated text
      "8201",                // truncated array
      "a161",                // truncated map key
      "a1f601",              // non-text map key
      "1bffffffffffffffff",  // uint64 > int64 max
      "3bffffffffffffffff",  // negative below int64 min
      "0001",                // trailing bytes
      "9f01",                // unterminated indefinite array
      "7f6161",              // unterminated indefinite string
      "c1f6",                // tag 1 with non-numeric content
      "fc",                  // reserved additional info
  };
  for (const char* hex : bad) {
    EXPECT_FALSE(Decode(FromHex(hex)).ok()) << hex;
  }
}

// Regression (fuzzer-found): a text/byte string whose declared length is near
// 2^64 must be rejected as truncated, not fed to string::assign. The old
// bounds check `pos + length > size` overflowed and wrapped past the guard.
TEST(CborTest, RejectsHugeStringLengthWithoutOverflow) {
  // 0x7b = text string, 8-byte length; 0xff...ff = 2^64 - 1 bytes claimed.
  EXPECT_FALSE(Decode(FromHex("7bffffffffffffffff")).ok());
  // 0x5b = byte string, same trick.
  EXPECT_FALSE(Decode(FromHex("5bffffffffffffffff")).ok());
  // Indefinite text string with a giant chunk length.
  EXPECT_FALSE(Decode(FromHex("7f7bffffffffffffffff")).ok());
}

TEST(CborTest, RejectsExcessiveNesting) {
  std::string hex;
  for (int i = 0; i < 100; ++i) hex += "81";  // 100 nested single-element arrays
  hex += "01";
  EXPECT_FALSE(Decode(FromHex(hex)).ok());
}

}  // namespace
}  // namespace opal::cbor
