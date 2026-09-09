#ifndef SMITHY_CBOR_CBOR_H_
#define SMITHY_CBOR_CBOR_H_

#include "smithy/core/blob.h"
#include "smithy/core/document.h"
#include "smithy/core/outcome.h"

namespace opal::cbor {

// Encodes a Document as deterministic CBOR (RFC 8949): smallest-width
// integers, definite lengths only, sorted map keys (inherent to DocumentMap),
// doubles for floating point, tag 1 for timestamp nodes (integer seconds when
// whole, double otherwise), byte strings for blobs.
Blob Encode(const Document& doc);

// Tolerant decoder: accepts indefinite-length strings/arrays/maps, half/
// single/double precision floats, and unknown tags (ignored, inner value
// kept). Tag 1 becomes a timestamp node. Rejects: map keys that are not text,
// integers outside int64 range, truncated or trailing input.
Outcome<Document> Decode(const Blob& bytes);

}  // namespace opal::cbor

#endif  // SMITHY_CBOR_CBOR_H_
