# Contributing to opal-cpp

Thanks for your interest! This project is in early development — see
[`docs/PLAN.md`](docs/PLAN.md) for the roadmap and [`docs/adr/`](docs/adr/) for the decisions
that shape it. [`docs/development.md`](docs/development.md) covers building and testing.

## Ground rules

- **Build system:** Bazel 9 only (ADR-0004). Install [bazelisk](https://github.com/bazelbuild/bazelisk);
  it picks up `.bazelversion` automatically. The `codegen/` JVM subproject builds with Gradle.
- **C++:** C++20, Google style via `.clang-format` (100-column limit), `.clang-tidy` clean.
  `Outcome`-based error handling — no exceptions across public API boundaries (ADR-0003).
- **Java:** 17+, formatted with google-java-format (enforced by Spotless: `gradle spotlessApply`).
- **Starlark:** formatted with `buildifier`; always `load()` C++ rules from `@rules_cc`.
- **Tests:** every change lands with tests. Run `bazel test //...` and
  `(cd codegen && gradle build)` before sending a PR.
- **Vendor neutrality:** no AWS-specific traits, auth, or SDK behaviors (PLAN §2).

## Pull requests

1. Fork/branch from `main`; keep PRs focused on one concern.
2. Make CI green: Bazel matrix (Linux gcc/clang, macOS), sanitizers, Gradle, lint.
3. Architectural changes need an ADR in `docs/adr/` (next sequential number).

## License

By contributing you agree that your contributions are licensed under the
[Apache License 2.0](LICENSE).
