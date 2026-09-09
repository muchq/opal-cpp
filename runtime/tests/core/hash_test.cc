// smithy/core/hash.h: HashCombine's mixing, HashValue's container dispatch,
// and the std::hash specializations for the runtime types generated members
// use (Blob, Timestamp, Unit) — the plumbing that lets generated types key
// std::unordered_map/std::unordered_set (issue #49).

#include "smithy/core/hash.h"

#include <gtest/gtest.h>

#include <concepts>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "smithy/core/blob.h"
#include "smithy/core/document.h"
#include "smithy/core/outcome.h"
#include "smithy/core/timestamp.h"

namespace opal {
namespace {

template <typename T>
concept Hashable = requires(const T& value) {
  { std::hash<T>{}(value) } -> std::convertible_to<std::size_t>;
};

// The runtime types generated members use are hashable...
static_assert(Hashable<Blob>);
static_assert(Hashable<Timestamp>);
static_assert(Hashable<Unit>);
// ... but Document deliberately is not: hashing follows ordering (a type gets
// std::hash exactly when it gets <=>), and Document is equality-only.
static_assert(!Hashable<Document>);

TEST(HashCombineTest, MixesOrderSensitively) {
  EXPECT_NE(HashCombine(HashCombine(0, 1), 2), HashCombine(HashCombine(0, 2), 1));
  EXPECT_NE(HashCombine(0, 7), std::size_t{7});
}

TEST(HashValueTest, VectorsHashByContentInOrder) {
  const std::vector<std::string> ab{"a", "b"};
  EXPECT_EQ(HashValue(ab), HashValue(std::vector<std::string>{"a", "b"}));
  EXPECT_NE(HashValue(ab), HashValue(std::vector<std::string>{"b", "a"}));
  EXPECT_NE(HashValue(ab), HashValue(std::vector<std::string>{"a"}));
}

TEST(HashValueTest, VectorBoolHashesThroughTheProxyReference) {
  // vector<bool> iteration yields a proxy, not bool& — and libc++'s proxy has
  // no std::hash (deleted; caught by macOS CI on PR #84). Elements must be
  // bound as value_type so the proxy decays to bool before dispatch.
  EXPECT_EQ(HashValue(std::vector<bool>{true, false}), HashValue(std::vector<bool>{true, false}));
  EXPECT_NE(HashValue(std::vector<bool>{true}), HashValue(std::vector<bool>{false}));
  const std::optional<std::vector<bool>> engaged{{true}};
  EXPECT_EQ(HashValue(engaged), HashValue(std::optional<std::vector<bool>>{{true}}));
}

TEST(HashValueTest, MapsHashByEntries) {
  const std::map<std::string, int> menu{{"drip", 3}, {"latte", 5}};
  EXPECT_EQ(HashValue(menu), HashValue(std::map<std::string, int>{{"latte", 5}, {"drip", 3}}));
  EXPECT_NE(HashValue(menu), HashValue(std::map<std::string, int>{{"drip", 3}}));
}

TEST(HashValueTest, OptionalsDistinguishAbsentFromEngaged) {
  EXPECT_EQ(HashValue(std::optional<int>{}), HashValue(std::optional<int>{}));
  EXPECT_EQ(HashValue(std::optional<int>{7}), HashValue(std::optional<int>{7}));
  EXPECT_NE(HashValue(std::optional<int>{}), HashValue(std::optional<int>{0}));
  // Nesting works even where std::hash has no specialization to lean on:
  // optional<vector> is exactly what @sparse-adjacent generated members carry.
  const std::optional<std::vector<int>> engaged{{1, 2}};
  EXPECT_EQ(HashValue(engaged), HashValue(std::optional<std::vector<int>>{{1, 2}}));
}

TEST(HashValueTest, ScalarsDeferToStdHash) {
  EXPECT_EQ(HashValue(std::string("x")), std::hash<std::string>{}(std::string("x")));
  EXPECT_EQ(HashValue(42), std::hash<int>{}(42));
}

TEST(StdHashTest, BlobHashesByBytes) {
  EXPECT_EQ(std::hash<Blob>{}(Blob::FromString("abc")), std::hash<Blob>{}(Blob::FromString("abc")));
  const std::unordered_set<Blob> set{Blob::FromString("a"), Blob::FromString("a"),
                                     Blob::FromString("b")};
  EXPECT_EQ(set.size(), 2u);
}

TEST(StdHashTest, TimestampHashesByInstant) {
  const auto at = [](std::int64_t ms) { return Timestamp::FromEpochMilliseconds(ms); };
  EXPECT_EQ(std::hash<Timestamp>{}(at(1000)), std::hash<Timestamp>{}(at(1000)));
  const std::unordered_set<Timestamp> set{at(1), at(1), at(2)};
  EXPECT_EQ(set.size(), 2u);
}

TEST(StdHashTest, UnitHashesConsistently) {
  EXPECT_EQ(std::hash<Unit>{}(Unit{}), std::hash<Unit>{}(Unit{}));
}

}  // namespace
}  // namespace opal
