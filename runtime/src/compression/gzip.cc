#include "smithy/compression/gzip.h"

#include "smithy/compression/gzip_test_peer.h"
#include "smithy/core/fatal.h"

// next_in becomes const Bytef*, so the feed below takes string_view bytes
// verbatim — no const_cast (ES.50, issue #109).
#define ZLIB_CONST
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace opal {

namespace {
constexpr int kGzipWindowBits = 15 + 16;  // 32KB window, gzip wrapper
constexpr std::size_t kChunk = std::size_t{16} * 1024;

// The public entry points' input-feed bound: zlib's avail_in is 32-bit
// (uInt), so input reaches it in slices no larger than this — a >4 GiB
// body arrives whole instead of through a silently truncating cast, which
// used to compress a 4 GiB + N byte body into a *valid* gzip stream of its
// first N bytes (issue #109). Any value that fits uInt is correct; 1 GiB
// keeps the arithmetic obviously clear of the boundary.
constexpr std::size_t kMaxFeed = std::size_t{1} << 30;
static_assert(kMaxFeed <= std::numeric_limits<uInt>::max());

// One deflateEnd/inflateEnd on every path out (R.1): the feed loops below
// have several early returns, and a missed teardown is a ~256 KB leak.
class ZStreamGuard {
 public:
  ZStreamGuard(z_stream& stream, int (*end)(z_stream*)) : stream_(&stream), end_(end) {}
  ~ZStreamGuard() { end_(stream_); }
  ZStreamGuard(const ZStreamGuard&) = delete;
  ZStreamGuard& operator=(const ZStreamGuard&) = delete;

 private:
  z_stream* stream_;
  int (*end_)(z_stream*);
};

// Arms the next input slice once zlib has drained the last one. Returns
// the total fed so far; the casts are lossless by the max_feed contract.
std::size_t FeedInput(z_stream& stream, std::string_view data, std::size_t fed,
                      std::size_t max_feed) {
  if (stream.avail_in != 0 || fed >= data.size()) return fed;
  const std::size_t feed = std::min(data.size() - fed, max_feed);
  stream.next_in = reinterpret_cast<const Bytef*>(data.data() + fed);
  stream.avail_in = static_cast<uInt>(feed);
  return fed + feed;
}

}  // namespace

namespace internal {

namespace {

// The seam's precondition, enforced rather than commented (ADR-0009): zero
// makes no progress — both loops would spin on Z_BUF_ERROR forever — and
// anything past uInt reintroduces the truncating cast at the seam itself.
void RequireFeedBound(std::size_t max_feed) {
  if (max_feed == 0 || max_feed > std::numeric_limits<uInt>::max()) {
    Fatal("gzip: max_feed must be in (0, UINT_MAX]");
  }
}

}  // namespace

Outcome<std::string> GzipCompressChunked(std::string_view data, std::size_t max_feed) {
  RequireFeedBound(max_feed);
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, kGzipWindowBits, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return Error::Serialization("gzip: deflateInit2 failed");
  }
  const ZStreamGuard guard(stream, deflateEnd);

  std::size_t fed = 0;
  std::string out;
  std::array<char, kChunk> buffer{};
  int result = Z_OK;
  do {
    fed = FeedInput(stream, data, fed, max_feed);
    // Z_FINISH only once every byte has been handed over; earlier slices
    // stream with Z_NO_FLUSH so deflate never sees a false end of input.
    const int flush = fed == data.size() ? Z_FINISH : Z_NO_FLUSH;
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = deflate(&stream, flush);
    // Only the expected codes may pass (decompress's posture): a stray
    // Z_MEM_ERROR must fail, not orbit the loop it can never finish.
    // Z_BUF_ERROR while input remains just means "feed me".
    const bool starved = result == Z_BUF_ERROR && stream.avail_in == 0 && fed < data.size();
    if (result != Z_OK && result != Z_STREAM_END && !starved) {
      return Error::Serialization("gzip: deflate failed");
    }
    out.append(buffer.data(), buffer.size() - stream.avail_out);
  } while (result != Z_STREAM_END);
  if (fed != data.size() || stream.avail_in != 0) {
    // Unreachable by zlib's contract (Z_STREAM_END consumes all input fed,
    // and Z_FINISH is only issued at the end); kept so the silent-truncation
    // failure class can never return silently.
    return Error::Serialization("gzip: input not fully consumed");
  }
  return out;
}

Outcome<std::string> GzipDecompressChunked(std::string_view data, std::size_t max_output,
                                           std::size_t max_feed) {
  RequireFeedBound(max_feed);
  z_stream stream{};
  if (inflateInit2(&stream, kGzipWindowBits) != Z_OK) {
    return Error::Serialization("gzip: inflateInit2 failed");
  }
  const ZStreamGuard guard(stream, inflateEnd);

  std::size_t fed = 0;
  std::string out;
  std::array<char, kChunk> buffer{};
  int result = Z_OK;
  do {
    fed = FeedInput(stream, data, fed, max_feed);
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = inflate(&stream, Z_NO_FLUSH);
    // Z_BUF_ERROR with input still queued just means "feed me" — the next
    // iteration does. With nothing left to feed it is a truncated stream.
    const bool starved = result == Z_BUF_ERROR && stream.avail_in == 0 && fed < data.size();
    if (result != Z_OK && result != Z_STREAM_END && !starved) {
      return Error::Serialization("gzip: malformed stream");
    }
    out.append(buffer.data(), buffer.size() - stream.avail_out);
    if (out.size() > max_output) {
      return Error::Serialization("gzip: output exceeds limit");
    }
  } while (result != Z_STREAM_END);
  if (fed != data.size() || stream.avail_in != 0) {
    // The whole input, not just the truncated 32-bit view of it, must
    // belong to the one stream (issue #109's second corner: a complete
    // member in the first slice must not silently orphan the rest).
    return Error::Serialization("gzip: trailing garbage after stream");
  }
  return out;
}

}  // namespace internal

Outcome<std::string> GzipCompress(std::string_view data) {
  return internal::GzipCompressChunked(data, kMaxFeed);
}

Outcome<std::string> GzipDecompress(std::string_view data, std::size_t max_output) {
  return internal::GzipDecompressChunked(data, max_output, kMaxFeed);
}

}  // namespace opal
