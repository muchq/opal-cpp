#!/usr/bin/env bash
# Asserts the dependency-resolution invariant behind opal_cpp's boringssl
# pin (see the comment above the bazel_dep in ../../MODULE.bazel): the
# version this consumer's module graph actually selects for boringssl is
# exactly the version opal_cpp declares. Bazel's MVS picks the highest
# version any module in the graph requests, so a transitive dependency could
# quietly drag the security-sensitive TLS library past the pinned-and-tested
# version — every CI leg would still be green while consumers link a
# boringssl no smithy-cpp job ever exercised. This check turns that silent
# skew into a failure. Run from anywhere; CI runs it in the consumer job
# after the test suite.
set -euo pipefail
cd "$(dirname "$0")"

bazel="${BAZEL:-$(command -v bazelisk || command -v bazel)}"

pinned=$(sed -n 's/^bazel_dep(name = "boringssl", version = "\([^"]*\)").*/\1/p' ../../MODULE.bazel)
if [[ -z "$pinned" ]]; then
  echo "error: no boringssl bazel_dep found in opal_cpp's MODULE.bazel" >&2
  exit 1
fi

resolved=$("$bazel" mod deps opal_cpp --output json |
  jq -r '.dependencies[] | select(.name == "opal_cpp")
         | .dependencies[] | select(.name == "boringssl") | .version')
if [[ -z "$resolved" ]]; then
  echo "error: boringssl is not among opal_cpp's resolved dependencies" >&2
  exit 1
fi

if [[ "$resolved" != "$pinned" ]]; then
  echo "error: the module graph resolved boringssl@$resolved, but opal_cpp pins $pinned" >&2
  echo "MVS selected a version opal_cpp's own CI never tested; either bump the" >&2
  echo "pin deliberately or find the module dragging boringssl forward." >&2
  exit 1
fi

echo "OK: resolved boringssl@$resolved matches opal_cpp's pin"
