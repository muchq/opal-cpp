#!/usr/bin/env bash
# Guards the version sources against drift and pins the CHANGELOG extraction
# the release workflow pipes into `gh release create`. Runs in the normal
# `bazel test //...` matrix, so a half-finished version bump fails on the PR
# rather than at tag time.
set -euo pipefail

RELEASE=tools/release.sh
failures=0

fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

expect_success() {
  local what=$1
  shift
  "$@" >/dev/null || fail "$what: expected success, got exit $?"
}

expect_failure() {
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then
    fail "$what: expected non-zero exit"
  fi
}

expect_success "check on the committed tree" "$RELEASE" check

# Smithy trait names are @-prefixed. Outside a code span GitHub renders them as
# user mentions, so the notes credit whoever owns that account: v0.1.0 shipped
# listing @required and @timestampFormat as contributors.
bare_mentions=$(sed 's/`[^`]*`//g' CHANGELOG.md | grep -n '@[A-Za-z]' || true)
if [[ -n $bare_mentions ]]; then
  fail "CHANGELOG.md has @-mentions outside code spans:
$bare_mentions"
fi

# Not a released version: the CHANGELOG heading exists while developing, but
# there are no notes to publish for it.
expect_failure "notes Unreleased" "$RELEASE" notes Unreleased
expect_failure "notes with no argument" "$RELEASE" notes
expect_failure "notes for an absent version" "$RELEASE" notes 99.99.99

# The workflow reads the version through this subcommand to decide whether the
# pushed tag names the commit it was pushed at.
version=$("$RELEASE" version)
[[ -n $version ]] || fail "version: printed nothing"

if [[ $version == *-dev ]]; then
  # Development state: the current version has no section of its own.
  expect_failure "notes for the -dev version" "$RELEASE" notes "$version"
else
  section=$("$RELEASE" notes "$version") || fail "notes $version: expected success"
  [[ -n ${section:-} ]] || fail "notes $version: empty section"
  # Stopping at the next heading is the whole contract — a greedy extraction
  # would ship every prior release's notes as the current release's.
  if grep -q '^## \[' <<<"${section:-}"; then
    fail "notes $version: ran into the next section"
  fi
  if grep -qE '^\[[^]]+\]: ' <<<"${section:-}"; then
    fail "notes $version: ran into the link footnotes"
  fi
fi

# bump rewrites the tree, so it runs against a throwaway copy of the files it
# touches.
release_abs="$PWD/$RELEASE"
work="${TEST_TMPDIR:-$(mktemp -d)}/bump"
mkdir -p "$work/runtime/src/core" "$work/runtime/tests/core" \
  "$work/runtime/include/opal/client" "$work/codegen" "$work/docs"
cp CHANGELOG.md "$work/"
cp runtime/src/core/version.cc "$work/runtime/src/core/"
cp runtime/tests/core/version_test.cc "$work/runtime/tests/core/"
cp runtime/include/opal/client/config.h "$work/runtime/include/opal/client/"
cp codegen/gradle.properties "$work/codegen/"
cp docs/versioning.md "$work/docs/"
cd "$work"

expect_failure "bump with no argument" "$release_abs" bump
expect_failure "bump to a release version" "$release_abs" bump 9.9.9
expect_failure "bump to a malformed version" "$release_abs" bump next

if [[ $version == *-dev ]]; then
  expect_failure "bump while already developing" "$release_abs" bump 9.9.9-dev
else
  expect_success "bump" "$release_abs" bump 9.9.9-dev
  # bump runs check itself, but a tree that passes check is the whole point.
  expect_success "check on the bumped tree" "$release_abs" check

  grep -q 'return "9.9.9-dev";' runtime/src/core/version.cc ||
    fail "bump: version.cc not rewritten"
  grep -q 'EXPECT_EQ(Version(), "9.9.9-dev");' runtime/tests/core/version_test.cc ||
    fail "bump: version_test.cc not rewritten"
  grep -q 'smithy-cpp/9.9.9-dev' runtime/include/opal/client/config.h ||
    fail "bump: config.h not rewritten"
  grep -qx 'version=9.9.9-dev' codegen/gradle.properties ||
    fail "bump: gradle.properties not rewritten"
  [[ $(grep -m1 '^## \[' CHANGELOG.md) == "## [Unreleased]" ]] ||
    fail "bump: CHANGELOG does not reopen with [Unreleased]"
  # Reopening must not swallow the release that was just cut.
  grep -q "^## \[$version\] - " CHANGELOG.md ||
    fail "bump: dropped the [$version] section"
  grep -q "^\[$version\]: " CHANGELOG.md ||
    fail "bump: dropped the [$version] link footnote"

  state_heading=$(grep -m1 '^## Current state' docs/versioning.md)
  [[ $state_heading == "## Current state: $version released, 9.9.9 in development" ]] ||
    fail "bump: versioning.md heading is '$state_heading'"
  grep -q '`9.9.9-dev`' docs/versioning.md ||
    fail "bump: versioning.md does not report the development version"
  # Swapping the section must consume exactly it: one heading left, and the
  # section after it still there.
  [[ $(grep -c '^## Current state' docs/versioning.md) -eq 1 ]] ||
    fail "bump: versioning.md has a duplicated Current state section"
  grep -q '^## Semantic versioning' docs/versioning.md ||
    fail "bump: versioning.md lost the section after Current state"
  # The policy prose inside the swapped section is not version-dependent, so
  # regenerating must not quietly drop it.
  grep -q 'Bazel Central Registry' docs/versioning.md ||
    fail "bump: versioning.md lost the bzlmod/BCR paragraph"

  expect_failure "bump twice" "$release_abs" bump 9.9.10-dev
fi

if ((failures > 0)); then
  echo "$failures assertion(s) failed" >&2
  exit 1
fi
echo "PASS"
