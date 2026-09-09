# ADR-0004: Bazel 9 is the only supported build system

**Status:** Accepted (2026-07-06)

## Context

The project needs a build story for the C++ runtime, for generated code, and for consumers.
Supporting both CMake and Bazel was considered and rejected during plan review: dual-build
parity is a permanent maintenance tax, and the target audience builds with Bazel.

## Decision

- **Bazel 9 (bzlmod-only)** is the sole supported build system, for this repository and for
  consumers. `.bazelversion` tracks `9.x`.
- Consumers use the `opal_cpp` bzlmod module (published to the Bazel Central Registry from
  Phase 6) and the `smithy_cpp_*_library` rules, which run the generator hermetically inside the
  build graph.
- No CMake files are provided or accepted. The `smithy-cpp` CLI (Phase 6) can emit plain C++
  sources plus a file manifest for vendoring into other build systems, as an explicitly
  unsupported escape hatch.
- The codegen JVM subproject builds with Gradle (standard for Smithy plugins) — that is an
  internal development tool, not a consumer-facing build system.

## Consequences

- One build system to keep green: CI runs `bazel test //...` on Linux (gcc, clang), macOS, and
  Windows (MSVC), plus sanitizer configs from `.bazelrc`.
- Third-party BUILD compatibility: Bazel 9 removed the native C++ rules, so `.bazelrc` sets
  `--incompatible_autoload_externally` for dependencies whose BUILD files predate the removal;
  our own BUILD files always `load()` from `@rules_cc` explicitly.
- Non-Bazel users are out of scope by design; revisiting this decision requires a new ADR.
