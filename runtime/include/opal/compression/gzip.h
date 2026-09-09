#ifndef OPAL_COMPRESSION_GZIP_H_
#define OPAL_COMPRESSION_GZIP_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "opal/core/outcome.h"

namespace opal {

// Gzip-compresses data (@requestCompression request bodies). Inputs of any
// size: bytes reach zlib's 32-bit counters in bounded slices, never through
// a truncating cast (issue #109).
Outcome<std::string> GzipCompress(std::string_view data);

// Decompresses a gzip stream, refusing outputs larger than max_output
// (decompression-bomb guard for server-side request bodies). The whole
// input must be the one stream — trailing bytes are an error.
Outcome<std::string> GzipDecompress(std::string_view data,
                                    std::size_t max_output = std::size_t{64} * 1024 * 1024);

}  // namespace opal

#endif  // OPAL_COMPRESSION_GZIP_H_
