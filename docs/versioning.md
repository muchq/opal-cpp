# Versioning and compatibility policy

smithy-cpp versions the **runtime** and the **generator** together — a
release is one git tag (`vX.Y.Z`, signed) covering both, and generated code
from generator X.Y is supported against runtime X.Y. `opal::Version()`
returns the runtime's version.

## Current state: 0.2.0 released, 0.3.0 in development

`v0.2.0` is the current release; `main` develops 0.3.0. The one product version
consumers observe — `opal::Version()` (`runtime/src/core/version.cc`) and the
client `User-Agent` (`opal::ClientConfig::user_agent`) — reports
**`0.3.0-dev`** on `main` until that tag lands, and the generator's Gradle
`version` (`codegen/gradle.properties`) tracks it, since the two ship under one
tag. The bzlmod **module** version in `MODULE.bazel` is a separate identifier
and stays `0.0.0` until the module is published to the Bazel Central Registry
(deferred, PLAN Phase 6); consumers override the module source with
`git_override`/`archive_override`, which ignores that value, so pin the
`v0.2.0` tag — the released one, not `main`.

## Semantic versioning

Pre-1.0 caveat (per semver): minor releases may break. Concretely:

- **Patch (0.1.x)**: bug fixes only. No changes to generated-code shape, wire
  behavior pinned by the conformance suites, or public runtime headers beyond
  fixes. Regenerating with a patch release produces compatible code.
- **Minor (0.x.0)**: may add features and may break — but breaking changes to
  the surfaces below must be called out in the CHANGELOG under a "Breaking"
  heading with a migration note.
- **Major (1.0.0 and later)**: the type-mapping contract, wire behavior, and
  public runtime API become stable; breaking changes require a major bump.

## What counts as the compatibility surface

1. **Wire behavior** — everything pinned by the conformance suites under
   `protocol-tests/`. A change that alters bytes on the wire for an
   unchanged model is breaking, full stop. The suites (and their must-shrink
   exclusion lists) are the executable definition.
2. **Generated-code shape** — the Smithy → C++ mapping contract in
   [generated-types.md](generated-types.md): type mappings, optionality
   rules, naming, error surfacing (`opal::Error` + typed details), the
   `<Service>Client` / `<Service>Handler` / `<Service>Server` interfaces.
   Consumers write code against generated headers; changes that break
   recompilation of handler implementations or client call sites are
   breaking.
3. **Public runtime headers** — `runtime/include/opal/**`. Generated code
   links against these, so removals/signature changes are breaking; pure
   additions are not.

Not covered: the *textual content* of generated `.cc` files (regeneration may
reshuffle internals freely), test-only targets (`//runtime:protocol_test_support`,
generated `tests/`), anything under `docs/`, and the generator's Java
internals (the `cpp-codegen` smithy-build plugin id and its settings schema
*are* covered).

## What a generator upgrade may change

Regenerating a model with a newer generator of the same minor version may
change generated file contents arbitrarily but must keep: the generated
public headers source-compatible for consumers, and the wire bytes identical
for unchanged models (golden regeneration in CI enforces byte-identical
output for the pinned generator; across generator versions the conformance
suites are the contract instead).

## Release mechanics

- Update `opal::Version()` + `CHANGELOG.md` in the release PR; CI must be
  fully green (full test matrix, consumer acceptance, all three protocol
  conformance suites). `//tools:release_test` fails the PR if the version
  sources disagree or the CHANGELOG's leading section doesn't match the state
  the version implies (`-dev` ⇒ `[Unreleased]`, otherwise `[X.Y.Z]`).
- Tag the merge commit `vX.Y.Z` with a **signed, annotated tag**; the tag
  message is the CHANGELOG section for the release
  (`tools/release.sh notes X.Y.Z` prints it). Pushing the tag runs
  [release.yml](../.github/workflows/release.yml), which re-checks the tree,
  refuses a lightweight tag or one pushed at a commit declaring a different
  version, and publishes the GitHub Release with those notes — followed by
  GitHub's generated list of the merged PRs since the previous release, so
  the CHANGELOG section carries the prose and the commit log comes for free.
  Signing stays local — Actions holds no key.
- BCR and Maven Central publishing remain deferred until production
  validation (PLAN Phase 6); consumers pin the tag via `git_override` /
  `archive_override` as shown in [quickstart.md](quickstart.md).
