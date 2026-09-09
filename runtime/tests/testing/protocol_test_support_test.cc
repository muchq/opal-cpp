// The generated protocol conformance suites stand on these helpers, so their
// semantics (multiset query matching, NaN-aware body equivalence) are pinned
// here first.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "smithy/testing/protocol_test.h"

namespace opal::testing {
namespace {

TEST(UriPathTest, SplitsTargetAtQuestionMark) {
  EXPECT_EQ(UriPath("/a/b?c=d"), "/a/b");
  EXPECT_EQ(UriPath("/a/b"), "/a/b");
  EXPECT_EQ(UriPath("/?x"), "/");
}

TEST(QueryEntriesTest, SplitsRawEntries) {
  EXPECT_EQ(QueryEntries("/p?a=b&c&d=e%26f"), (std::vector<std::string>{"a=b", "c", "d=e%26f"}));
  EXPECT_TRUE(QueryEntries("/p").empty());
}

TEST(QueryContainsTest, MultisetSemantics) {
  EXPECT_TRUE(QueryContains("/p?a=1&a=2&b=3", {"a=1", "a=2"}));
  EXPECT_TRUE(QueryContains("/p?a=1&b=2", {}));
  EXPECT_FALSE(QueryContains("/p?a=1", {"a=1", "a=1"})) << "duplicates must be counted";
  EXPECT_FALSE(QueryContains("/p?a=1", {"a=2"}));
}

TEST(QueryKeysTest, ForbidAndRequire) {
  EXPECT_TRUE(QueryForbidsKeys("/p?a=1", {"b"}));
  EXPECT_FALSE(QueryForbidsKeys("/p?a=1&b", {"b"}));
  EXPECT_TRUE(QueryRequiresKeys("/p?a=1&b", {"a", "b"}));
  EXPECT_FALSE(QueryRequiresKeys("/p?a=1", {"b"}));
}

TEST(ProtocolDocumentEqualsTest, NumbersCompareAcrossRepresentations) {
  EXPECT_TRUE(ProtocolDocumentEquals(Document(std::int64_t{5}), Document(5.0)));
  EXPECT_FALSE(ProtocolDocumentEquals(Document(std::int64_t{5}), Document(5.5)));
  // Large int64 values keep exact comparison when both sides are ints.
  const std::int64_t big = 9223372036854775807LL;
  EXPECT_TRUE(ProtocolDocumentEquals(Document(big), Document(big)));
  EXPECT_FALSE(ProtocolDocumentEquals(Document(big), Document(big - 1)));
}

TEST(ProtocolDocumentEqualsTest, NanEqualsNan) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(ProtocolDocumentEquals(Document(nan), Document(nan)));
  EXPECT_FALSE(ProtocolDocumentEquals(Document(nan), Document(1.0)));
  DocumentMap a;
  a.emplace("v", Document(nan));
  DocumentMap b;
  b.emplace("v", Document(nan));
  EXPECT_TRUE(ProtocolDocumentEquals(Document(std::move(a)), Document(std::move(b))));
}

TEST(ProtocolDocumentEqualsTest, StructuralRecursion) {
  DocumentList la;
  la.emplace_back(Document("x"));
  DocumentList lb;
  lb.emplace_back(Document("x"));
  EXPECT_TRUE(ProtocolDocumentEquals(Document(std::move(la)), Document(std::move(lb))));
  DocumentMap ma;
  ma.emplace("k", Document(true));
  DocumentMap mb;
  mb.emplace("k", Document(false));
  EXPECT_FALSE(ProtocolDocumentEquals(Document(std::move(ma)), Document(std::move(mb))));
}

TEST(JsonBodyEqualsTest, IgnoresFormattingAndKeyOrder) {
  EXPECT_TRUE(JsonBodyEquals(R"({"a": 1, "b": [true]})", R"({"b":[true],"a":1})"));
  EXPECT_FALSE(JsonBodyEquals(R"({"a": 1})", R"({"a": 2})"));
  EXPECT_FALSE(JsonBodyEquals(R"({"a": 1})", "not json"));
}

TEST(CborBodyEqualsTest, ComparesDecodedDocuments) {
  DocumentMap map;
  map.emplace("a", Document(std::int64_t{1}));
  const std::string bytes = opal::cbor::Encode(Document(std::move(map))).ToString();
  const std::string b64 = opal::Base64Encode(opal::Blob::FromString(bytes));
  EXPECT_TRUE(CborBodyEqualsBase64(b64, bytes));
  EXPECT_FALSE(CborBodyEqualsBase64(b64, "junk"));
}

TEST(BodyMessageMatchesTest, MatchesJsonMessageMember) {
  EXPECT_TRUE(BodyMessageMatches("expected `true`", R"({"message": "found expected `true`"})"));
  EXPECT_FALSE(BodyMessageMatches("expected `true`", R"({"message": "something else"})"));
  EXPECT_FALSE(BodyMessageMatches(".*", R"({"no_message": 1})"));
  EXPECT_FALSE(BodyMessageMatches(".*", "not json"));
}

TEST(MutatingTransportTest, MutatesSuccessfulResponses) {
  auto inner = std::make_shared<CapturingTransport>();
  inner->next_response = {200, {}, "before"};
  MutatingTransport transport(inner,
                              [](opal::http::HttpResponse& response) { response.body += "!"; });
  const auto response = transport.Send({});
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->body, "before!");
}

TEST(CapturingTransportTest, RecordsAndReplays) {
  CapturingTransport transport;
  transport.next_response = {418, {}, "teapot"};
  opal::http::HttpRequest request;
  request.method = "PUT";
  const auto response = transport.Send(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->status, 418);
  EXPECT_EQ(transport.last_request.method, "PUT");
}

}  // namespace
}  // namespace opal::testing
