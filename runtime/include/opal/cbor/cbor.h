#ifndef OPAL_CBOR_CBOR_H_
#define OPAL_CBOR_CBOR_H_

#include "opal/core/blob.h"
#include "opal/core/document.h"
#include "opal/core/outcome.h"

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

#endif  // OPAL_CBOR_CBOR_H_
