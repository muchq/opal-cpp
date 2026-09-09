#include "smithy/compression/gzip.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "smithy/compression/gzip_test_peer.h"

namespace opal {
namespace {

// A body that compresses but not trivially, so slice boundaries land on
// varied byte patterns rather than one run of a single character.
std::string MixedBody(std::size_t size) {
  std::string body;
  body.reserve(size);
  while (body.size() < size) {
    body += "lorem ipsum #" + std::to_string(body.size() * 2654435761U % 9973) + ";";
  }
  body.resize(size);
  return body;
}

TEST(GzipTest, RoundTrips) {
  const std::string text(100000, 'a');
  const auto compressed = GzipCompress(text);
  ASSERT_TRUE(compressed.ok());
  EXPECT_LT(compressed->size(), text.size() / 10);  // trivially compressible
  const auto restored = GzipDecompress(*compressed);
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(*restored, text);
}

TEST(GzipTest, RoundTripsEmpty) {
  const auto compressed = GzipCompress("");
  ASSERT_TRUE(compressed.ok());
  const auto restored = GzipDecompress(*compressed);
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(*restored, "");
}

TEST(GzipTest, RejectsGarbage) {
  EXPECT_FALSE(GzipDecompress("definitely not gzip").ok());
  EXPECT_FALSE(GzipDecompress("").ok());
}

TEST(GzipTest, EnforcesOutputLimit) {
  const std::string text(100000, 'a');
  const auto compressed = GzipCompress(text);
  ASSERT_TRUE(compressed.ok());
  EXPECT_FALSE(GzipDecompress(*compressed, 1024).ok());
}

TEST(GzipTest, RejectsTrailingGarbage) {
  const auto compressed = GzipCompress("payload");
  ASSERT_TRUE(compressed.ok());
  EXPECT_FALSE(GzipDecompress(*compressed + "extra").ok());
}

// The issue #109 A-1 class: zlib's avail_in is 32-bit, so the fix feeds
// input in bounded slices. A 4 GiB fixture is not CI-realistic; instead the
// internal seam shrinks the feed bound to a few bytes, so these tests walk
// the exact re-feed loop a >4 GiB body walks — dozens of slices per call —
// with kilobyte inputs.

TEST(GzipChunkedFeedTest, RoundTripsAcrossTinyFeedSlices) {
  const std::string body = MixedBody(10000);
  const auto compressed = internal::GzipCompressChunked(body, /*max_feed=*/7);
  ASSERT_TRUE(compressed.ok()) << compressed.error().message();
  const auto restored =
      internal::GzipDecompressChunked(*compressed, /*max_output=*/1 << 20, /*max_feed=*/5);
  ASSERT_TRUE(restored.ok()) << restored.error().message();
  EXPECT_EQ(*restored, body);
}

TEST(GzipChunkedFeedTest, ChunkedAndUnchunkedStreamsInterchange) {
  // Chunked compression is readable by the ordinary path and vice versa:
  // slicing changes how bytes reach zlib, never the stream contract.
  const std::string body = MixedBody(4096);
  const auto chunked = internal::GzipCompressChunked(body, /*max_feed=*/3);
  ASSERT_TRUE(chunked.ok());
  const auto from_chunked = GzipDecompress(*chunked);
  ASSERT_TRUE(from_chunked.ok());
  EXPECT_EQ(*from_chunked, body);

  const auto whole = GzipCompress(body);
  ASSERT_TRUE(whole.ok());
  const auto sliced_read =
      internal::GzipDecompressChunked(*whole, /*max_output=*/1 << 20, /*max_feed=*/1);
  ASSERT_TRUE(sliced_read.ok());
  EXPECT_EQ(*sliced_read, body);
}

TEST(GzipChunkedFeedTest, EmptyInputSurvivesTheSmallestFeedBound) {
  const auto compressed = internal::GzipCompressChunked("", /*max_feed=*/1);
  ASSERT_TRUE(compressed.ok());
  const auto restored =
      internal::GzipDecompressChunked(*compressed, /*max_output=*/1024, /*max_feed=*/1);
  ASSERT_TRUE(restored.ok());
  EXPECT_EQ(*restored, "");
}

TEST(GzipChunkedFeedTest, TrailingGarbageArrivingInALaterSliceIsStillRejected) {
  // The stream ends several slices before the input does — the pre-fix
  // check only saw the truncated tail and would have called this clean.
  const auto compressed = GzipCompress("payload");
  ASSERT_TRUE(compressed.ok());
  const auto read = internal::GzipDecompressChunked(*compressed + "trailing garbage",
                                                    /*max_output=*/1024, /*max_feed=*/4);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error().kind(), ErrorKind::kSerialization);
}

TEST(GzipChunkedFeedTest, ATruncatedStreamAcrossSlicesIsMalformedNotAHang) {
  const auto compressed = GzipCompress(MixedBody(4096));
  ASSERT_TRUE(compressed.ok());
  const std::string truncated = compressed->substr(0, compressed->size() / 2);
  const auto read =
      internal::GzipDecompressChunked(truncated, /*max_output=*/1 << 20, /*max_feed=*/3);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error().kind(), ErrorKind::kSerialization);
}

TEST(GzipChunkedFeedTest, OutputLimitStillTripsMidStreamUnderSlicedFeeds) {
  const std::string body = MixedBody(100000);
  const auto compressed = internal::GzipCompressChunked(body, /*max_feed=*/64);
  ASSERT_TRUE(compressed.ok());
  const auto read =
      internal::GzipDecompressChunked(*compressed, /*max_output=*/1024, /*max_feed=*/64);
  ASSERT_FALSE(read.ok());
}

TEST(GzipChunkedFeedTest, GarbageUnderSlicedFeedsIsMalformedNotAHang) {
  const auto read = internal::GzipDecompressChunked("definitely not gzip",
                                                    /*max_output=*/1024, /*max_feed=*/2);
  ASSERT_FALSE(read.ok());
}

}  // namespace
}  // namespace opal
