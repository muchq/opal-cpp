"""Warning flags for every first-party C++ target (issue #65).

Hand-written BUILD files load OPAL_COPTS; the generated modules' BUILD
files (BuildFileGenerator / TestsBuildFileGenerator) and the consumer macros
in defs.bzl carry the same flags as literals, so generated code compiles at
the same warning level in-tree and in consumers.

-Werror deliberately stays out: CI promotes warnings to errors for repo code
only via --config=werror (see .bazelrc), so a new compiler version's new
warnings never break consumers building from source.
"""

OPAL_COPTS = [
    "-Wall",
    "-Wextra",
]
