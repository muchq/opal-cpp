// A curated hostile-input bank for the CBOR decoder, the counterpart of the
// vendored JSONTestSuite corpus the JSON parser runs against: every named
// malformation class from RFC 8949 §appendix-F plus the decoder's own
// documented limits. Complements the inline cases in cbor_test.cc (which pin
// specific regressions); this bank aims for systematic coverage — every
// multi-byte header truncated, every reserved encoding, indefinite-length
// abuse, depth bombs — so decoder changes get judged against the full
// hostile surface, not the handful of shapes the fuzzer happened to find.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "opal/cbor/cbor.h"

namespace opal::cbor {
namespace {

Blob FromHex(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(
        static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return Blob(std::move(bytes));
}

struct Vector {
  const char* hex;
  const char* why;
};

// --- Must be rejected, never crash, hang, or over-read -----------------

constexpr Vector kTruncatedHeaders[] = {
    {"18", "uint, 1-byte argument missing"},
    {"19", "uint, 2-byte argument missing"},
    {"1900", "uint, 2-byte argument half present"},
    {"1a", "uint, 4-byte argument missing"},
    {"1a0000", "uint, 4-byte argument half present"},
    {"1b", "uint, 8-byte argument missing"},
    {"1b00000000000000", "uint, 8-byte argument one byte short"},
    {"38", "negative, 1-byte argument missing"},
    {"3b00", "negative, 8-byte argument truncated"},
    {"58", "byte string, 1-byte length missing"},
    {"5b", "byte string, 8-byte length missing"},
    {"78", "text, 1-byte length missing"},
    {"79", "text, 2-byte length missing"},
    {"7a000000", "text, 4-byte length truncated"},
    {"98", "array, 1-byte count missing"},
    {"9b", "array, 8-byte count missing"},
    {"b8", "map, 1-byte count missing"},
    {"bb0000", "map, 8-byte count truncated"},
    {"d8", "tag, 1-byte number missing"},
    {"f8", "simple, 1-byte value missing"},
    {"f9", "half float, payload missing"},
    {"f97c", "half float, payload half present"},
    {"fa000000", "single float, payload truncated"},
    {"fb00000000000000", "double float, payload one byte short"},
};

constexpr Vector kTruncatedBodies[] = {
    {"41", "byte string claims 1 byte, has none"},
    {"58ff", "byte string claims 255 bytes, has none"},
    {"61", "text claims 1 byte, has none"},
    {"6161ff", "trailing byte after complete text"},
    {"62c3", "text ends mid UTF-8 sequence and short of its length"},
    {"820102ff", "trailing break after complete array"},
    {"81", "array claims 1 element, has none"},
    {"830102", "array claims 3 elements, has 2"},
    {"a1", "map claims 1 pair, has none"},
    {"a2616101", "map claims 2 pairs, has 1"},
    {"a16161", "map key present, value missing"},
    {"c0", "tag 0 with no content"},
    {"c1", "tag 1 with no content"},
};

constexpr Vector kIndefiniteAbuse[] = {
    {"5f", "indefinite byte string never terminated"},
    {"7f", "indefinite text never terminated"},
    {"9f", "indefinite array never terminated"},
    {"bf", "indefinite map never terminated"},
    {"9f9f01ff", "inner indefinite array closed, outer not"},
    {"bf61610102", "indefinite map missing its break"},
    {"bf6161ff", "indefinite map break after key, value missing"},
    {"ff", "break with no open container"},
    {"81ff", "break as a definite array's element"},
    {"a16161ff", "break as a definite map's value"},
    {"7f4161ff", "byte-string chunk inside indefinite text"},
    {"5f6161ff", "text chunk inside indefinite byte string"},
    {"7f7f6161ffff", "nested indefinite text (chunks must be definite)"},
    {"7f01ff", "integer chunk inside indefinite text"},
    {"9fc1ff", "break as tag 1 content inside indefinite array"},
    {"1f", "additional info 31 on uint (no indefinite integers)"},
    {"3f", "additional info 31 on negative"},
    {"df", "additional info 31 on tag"},
    {"df01", "additional info 31 on tag, content present"},
};

constexpr Vector kReservedEncodings[] = {
    {"1c", "reserved additional info 28 on uint"},
    {"1d", "reserved additional info 29 on uint"},
    {"1e", "reserved additional info 30 on uint"},
    {"3c", "reserved additional info 28 on negative"},
    {"5c", "reserved additional info 28 on byte string"},
    {"7c", "reserved additional info 28 on text"},
    {"9c", "reserved additional info 28 on array"},
    {"bc", "reserved additional info 28 on map"},
    {"dc", "reserved additional info 28 on tag"},
    {"fc", "reserved additional info 28 on simple/float"},
    {"fd", "reserved additional info 29 on simple/float"},
    {"fe", "reserved additional info 30 on simple/float"},
    {"f800", "two-byte simple value below 32"},
    {"f81f", "two-byte simple value 31 (reserved)"},
};

constexpr Vector kDomainLimits[] = {
    {"1bffffffffffffffff", "uint64 max exceeds the int64 document model"},
    {"1b8000000000000000", "2^63 exceeds int64 max"},
    {"3bffffffffffffffff", "-2^64 below int64 min"},
    {"3b8000000000000000", "-2^63-1 below int64 min"},
    {"a10102", "integer map key"},
    {"a1810001", "array map key"},
    {"a1a0616101", "map map key"},
    {"a1f4616101", "boolean map key"},
    {"a1f6616101", "null map key"},
    {"5bffffffffffffffff", "byte string claims 2^64-1 bytes (overflow guard)"},
    {"7bffffffffffffffff", "text claims 2^64-1 bytes (overflow guard)"},
    {"9bffffffffffffffff", "array claims 2^64-1 elements"},
    {"bbffffffffffffffff", "map claims 2^64-1 pairs"},
    {"c1616161", "tag 1 (timestamp) on non-numeric content"},
    {"c17f6161ff", "tag 1 on indefinite text"},
};

std::string Repeat(std::string_view unit, int n, std::string_view tail) {
  std::string hex;
  for (int i = 0; i < n; ++i) hex += unit;
  hex += tail;
  return hex;
}

TEST(CborHostileTest, RejectsEveryMalformedVector) {
  const auto check = [](const Vector* bank, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      EXPECT_FALSE(Decode(FromHex(bank[i].hex)).ok()) << bank[i].hex << ": " << bank[i].why;
    }
  };
  check(kTruncatedHeaders, std::size(kTruncatedHeaders));
  check(kTruncatedBodies, std::size(kTruncatedBodies));
  check(kIndefiniteAbuse, std::size(kIndefiniteAbuse));
  check(kReservedEncodings, std::size(kReservedEncodings));
  check(kDomainLimits, std::size(kDomainLimits));
}

TEST(CborHostileTest, RejectsDepthBombs) {
  // The decoder caps nesting at 64; each shape that recurses must hit the
  // cap instead of the process stack.
  EXPECT_FALSE(Decode(FromHex(Repeat("81", 65, "01"))).ok()) << "definite arrays";
  EXPECT_FALSE(Decode(FromHex(Repeat("9f", 65, "01") + Repeat("ff", 65, ""))).ok())
      << "indefinite arrays";
  EXPECT_FALSE(Decode(FromHex(Repeat("a16161", 65, "01"))).ok()) << "definite maps";
  EXPECT_FALSE(Decode(FromHex(Repeat("c6", 65, "01"))).ok()) << "tag chains";
  EXPECT_FALSE(Decode(FromHex(Repeat("81", 4096, "01"))).ok()) << "deep bomb, still bounded";
}

// --- Valid but nasty: must decode, not be over-rejected ----------------

TEST(CborHostileTest, AcceptsBoundaryIntegers) {
  const auto max = Decode(FromHex("1b7fffffffffffffff"));
  ASSERT_TRUE(max.ok());
  EXPECT_EQ(max->as_int(), std::int64_t{0x7fffffffffffffff});
  const auto min = Decode(FromHex("3b7fffffffffffffff"));
  ASSERT_TRUE(min.ok());
  EXPECT_EQ(min->as_int(), std::numeric_limits<std::int64_t>::min());
}

TEST(CborHostileTest, AcceptsHalfPrecisionEdgeCases) {
  const struct {
    const char* hex;
    double expected;
  } finite[] = {
      {"f90000", 0.0},
      {"f98000", -0.0},
      {"f90001", 5.960464477539063e-8},  // smallest subnormal
      {"f97bff", 65504.0},               // largest finite half
  };
  for (const auto& c : finite) {
    const auto doc = Decode(FromHex(c.hex));
    ASSERT_TRUE(doc.ok()) << c.hex;
    EXPECT_EQ(doc->as_double(), c.expected) << c.hex;
  }
  const auto inf = Decode(FromHex("f97c00"));
  ASSERT_TRUE(inf.ok());
  EXPECT_TRUE(std::isinf(inf->as_double()));
  const auto nan = Decode(FromHex("f97e00"));
  ASSERT_TRUE(nan.ok());
  EXPECT_TRUE(std::isnan(nan->as_double()));
}

TEST(CborHostileTest, AcceptsNestingAtTheDocumentedLimitOnly) {
  // 63 wrappers around a scalar sit inside the 64-frame budget; 65 must not.
  EXPECT_TRUE(Decode(FromHex(Repeat("81", 63, "01"))).ok());
  EXPECT_FALSE(Decode(FromHex(Repeat("81", 65, "01"))).ok());
}

TEST(CborHostileTest, AcceptsUnknownTagsAndKeepsTheInnerValue) {
  const auto tagged = Decode(FromHex("d82a6161"));  // tag 42 around "a"
  ASSERT_TRUE(tagged.ok());
  EXPECT_EQ(tagged->as_string(), "a");
}

TEST(CborHostileTest, AcceptsIndefiniteEverythingWithinLimits) {
  // {"a": [_ "b", {_ "c": h'00'}]} with every container indefinite.
  const auto doc = Decode(FromHex("bf61619f6162bf61635f4100ffffffff"));
  ASSERT_TRUE(doc.ok()) << (doc.ok() ? "" : doc.error().message());
  ASSERT_TRUE(doc->is_map());
}

TEST(CborHostileTest, DuplicateMapKeysDecodeWithoutFault) {
  const auto doc = Decode(FromHex("a2616101616102"));  // {"a": 1, "a": 2}
  ASSERT_TRUE(doc.ok());
  ASSERT_TRUE(doc->is_map());
  EXPECT_NE(doc->Find("a"), nullptr);
}

// --- The JSONTestSuite-style structural property ------------------------

// Every strict prefix of a valid document is itself malformed (CBOR is a
// prefix-free code for a single data item): the decoder must reject all of
// them rather than silently succeed on partial input.
TEST(CborHostileTest, EveryStrictPrefixOfAValidDocumentIsRejected) {
  const char* valid[] = {
      "1b7fffffffffffffff",                // int64 max
      "fb3ff199999999999a",                // 1.1
      "6449455446",                        // "IETF"
      "4401020304",                        // h'01020304'
      "83010203",                          // [1, 2, 3]
      "a26161016162820203",                // {"a": 1, "b": [2, 3]}
      "c11a514b67b0",                      // tag 1 timestamp
      "bf61619f6162bf61635f4100ffffffff",  // indefinite nest
  };
  for (const char* hex : valid) {
    const Blob full = FromHex(hex);
    ASSERT_TRUE(Decode(full).ok()) << hex;
    const auto& bytes = full.bytes();
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
      Blob prefix(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + cut));
      EXPECT_FALSE(Decode(prefix).ok()) << hex << " cut to " << cut << " bytes";
    }
  }
}

}  // namespace
}  // namespace opal::cbor
