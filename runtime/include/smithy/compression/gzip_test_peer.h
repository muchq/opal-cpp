#ifndef SMITHY_COMPRESSION_GZIP_TEST_PEER_H_
#define SMITHY_COMPRESSION_GZIP_TEST_PEER_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "smithy/core/outcome.h"

// The gzip feed-size seam, split out of gzip.h so the production surface
// stays two functions: these exist for the tests and fuzz harnesses, which
// shrink the slice bound to a few bytes and so walk the >4 GiB re-feed
// loop (issue #109) with kilobyte fixtures. Production code has no reason
// to include this header — the public entry points already pass the one
// correct bound.

namespace opal::internal {

// Preconditions (ADR-0009, enforced fatally): 0 < max_feed <= the largest
// value zlib's 32-bit avail_in can carry. Zero cannot make progress and a
// larger bound would reintroduce the truncating cast this seam exists to
// prevent.
Outcome<std::string> GzipCompressChunked(std::string_view data, std::size_t max_feed);
Outcome<std::string> GzipDecompressChunked(std::string_view data, std::size_t max_output,
                                           std::size_t max_feed);

}  // namespace opal::internal

#endif  // SMITHY_COMPRESSION_GZIP_TEST_PEER_H_
