// opal/core/print.h: DebugAppend's scalar/container rendering plus the
// AppendDebugTo members on the runtime types generated members use (Blob,
// Timestamp, Document, Unit, Boxed) — the plumbing behind generated
// DebugString()/operator<< (issue #85). Debug output is for humans: never
// parse it or pin it across library versions (these pins ARE the versioned
// definition, updated deliberately).

#include "opal/core/print.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "opal/core/blob.h"
#include "opal/core/boxed.h"
#include "opal/core/document.h"
#include "opal/core/outcome.h"
#include "opal/core/timestamp.h"

namespace opal {
namespace {

template <typename T>
std::string Printed(const T& value) {
  return DebugString(value);
}

TEST(DebugAppendTest, ScalarsRender) {
  EXPECT_EQ(Printed(true), "true");
  EXPECT_EQ(Printed(false), "false");
  EXPECT_EQ(Printed(42), "42");
  EXPECT_EQ(Printed(std::int64_t{-7}), "-7");
  EXPECT_EQ(Printed(2.5), "2.5");
  EXPECT_EQ(Printed(2.5F), "2.5");
}

TEST(DebugAppendTest, StringsQuoteAndEscape) {
  EXPECT_EQ(Printed(std::string("hello")), "\"hello\"");
  EXPECT_EQ(Printed(std::string("a\"b\\c")), "\"a\\\"b\\\\c\"");
  EXPECT_EQ(Printed(std::string("line\nend\t")), "\"line\\nend\\t\"");
  EXPECT_EQ(Printed(std::string("bell\x01")), "\"bell\\x01\"");
}

TEST(DebugAppendTest, ContainersRenderElementWise) {
  EXPECT_EQ(Printed(std::vector<int>{1, 2}), "[1, 2]");
  EXPECT_EQ(Printed(std::vector<int>{}), "[]");
  EXPECT_EQ(Printed(std::vector<bool>{true, false}), "[true, false]");  // proxy iterators (#84)
  const std::map<std::string, int> menu{{"drip", 3}, {"latte", 5}};
  EXPECT_EQ(Printed(menu), "{\"drip\": 3, \"latte\": 5}");
}

TEST(DebugAppendTest, OptionalsRenderValueOrNullopt) {
  EXPECT_EQ(Printed(std::optional<int>{7}), "7");
  EXPECT_EQ(Printed(std::optional<int>{}), "nullopt");
  // Sparse-list shape: optional elements inside a vector.
  EXPECT_EQ(Printed(std::vector<std::optional<int>>{1, std::nullopt}), "[1, nullopt]");
}

TEST(RuntimeTypePrintingTest, BlobRendersSizeAndBoundedHex) {
  EXPECT_EQ(Printed(Blob::FromString("abc")), "Blob(3 bytes: 616263)");
  EXPECT_EQ(Printed(Blob{}), "Blob(0 bytes)");
  // Contents are capped, not dumped: 16 hex bytes then an ellipsis.
  EXPECT_EQ(Printed(Blob::FromString("0123456789abcdefXYZ")),
            "Blob(19 bytes: 30313233343536373839616263646566…)");
}

TEST(RuntimeTypePrintingTest, TimestampRendersRfc3339) {
  EXPECT_EQ(Printed(Timestamp::FromEpochMilliseconds(0)), "Timestamp(1970-01-01T00:00:00Z)");
  EXPECT_EQ(Printed(Timestamp::FromEpochMilliseconds(1398796238520)),
            "Timestamp(2014-04-29T18:30:38.52Z)");
}

TEST(RuntimeTypePrintingTest, UnitRenders) { EXPECT_EQ(Printed(Unit{}), "Unit{}"); }

TEST(RuntimeTypePrintingTest, BoxedForwardsToTheValue) {
  EXPECT_EQ(Printed(Boxed<std::string>(std::string("deep"))), "\"deep\"");
}

TEST(RuntimeTypePrintingTest, DocumentRendersJsonish) {
  EXPECT_EQ(Printed(Document{}), "null");
  EXPECT_EQ(Printed(Document(7)), "7");
  EXPECT_EQ(Printed(Document(2.5)), "2.5");
  EXPECT_EQ(Printed(Document("text")), "\"text\"");
  DocumentMap map;
  map.emplace("name", Document("Seattle"));
  map.emplace("tags", Document(DocumentList{Document(1), Document(true)}));
  EXPECT_EQ(Printed(Document(std::move(map))), "{\"name\": \"Seattle\", \"tags\": [1, true]}");
  // Blob and timestamp nodes reuse their own renderings.
  EXPECT_EQ(Printed(Document(Blob::FromString("ab"))), "Blob(2 bytes: 6162)");
  EXPECT_EQ(Printed(Document::FromTimestamp(Timestamp::FromEpochMilliseconds(0),
                                            TimestampFormat::kDateTime)),
            "Timestamp(1970-01-01T00:00:00Z)");
}

}  // namespace
}  // namespace opal
