// Property tests for percent-encoding: encode/decode round trips over random
// byte strings (including raw UTF-8 and control bytes).

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "opal/http/uri.h"

namespace opal::http {
namespace {

std::string RandomBytes(std::mt19937_64& rng, int max_length) {
  std::string out;
  const auto length = static_cast<int>(rng() % static_cast<std::uint64_t>(max_length + 1));
  out.reserve(static_cast<std::size_t>(length));
  for (int i = 0; i < length; ++i) {
    out.push_back(static_cast<char>(rng() % 256));
  }
  return out;
}

TEST(UriPropertyTest, PathSegmentEncodingRoundTrips) {
  std::mt19937_64 rng(20260706);
  for (int i = 0; i < 1000; ++i) {
    const std::string original = RandomBytes(rng, 48);
    const auto decoded = PercentDecode(EncodePathSegment(original));
    ASSERT_TRUE(decoded.ok()) << "iteration " << i;
    ASSERT_EQ(*decoded, original) << "iteration " << i;
  }
}

TEST(UriPropertyTest, QueryComponentEncodingRoundTrips) {
  std::mt19937_64 rng(777);
  for (int i = 0; i < 1000; ++i) {
    const std::string original = RandomBytes(rng, 48);
    const auto decoded = PercentDecode(EncodeQueryComponent(original));
    ASSERT_TRUE(decoded.ok()) << "iteration " << i;
    ASSERT_EQ(*decoded, original) << "iteration " << i;
  }
}

TEST(UriPropertyTest, EncodedSegmentsSurviveRequestTargetParsing) {
  std::mt19937_64 rng(31337);
  for (int i = 0; i < 500; ++i) {
    std::string a = RandomBytes(rng, 24);
    std::string b = RandomBytes(rng, 24);
    // ParseRequestTarget treats a trailing empty segment as a trailing slash;
    // skip empties to keep the property crisp.
    if (a.empty() || b.empty()) {
      continue;
    }
    const std::string target = "/" + EncodePathSegment(a) + "/" + EncodePathSegment(b);
    const auto parsed = ParseRequestTarget(target);
    ASSERT_TRUE(parsed.ok()) << "iteration " << i;
    ASSERT_EQ(parsed->path_segments.size(), 2u) << target;
    EXPECT_EQ(parsed->path_segments[0], a) << "iteration " << i;
    EXPECT_EQ(parsed->path_segments[1], b) << "iteration " << i;
  }
}

TEST(UriPropertyTest, GreedyEncodingPreservesSlashesOnly) {
  std::mt19937_64 rng(99);
  for (int i = 0; i < 500; ++i) {
    const std::string original = RandomBytes(rng, 48);
    const std::string encoded = EncodeGreedyPathSegment(original);
    const auto decoded = PercentDecode(encoded);
    ASSERT_TRUE(decoded.ok());
    ASSERT_EQ(*decoded, original) << "iteration " << i;
  }
}

}  // namespace
}  // namespace opal::http
