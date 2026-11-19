#!/usr/bin/env bash
# Every pinned jar must be fetchable from a host outside Maven Central's own
# CDN: Central intermittently 404s existing jars to CI runners, and
# repo1.maven.org and repo.maven.apache.org fail together. The fallback URL
# only exists when the pin lists the artifact under each host.
set -euo pipefail

LOCK=${1:-maven_install.json}

# Emit "<repo-url> <artifact>" for each entry of the top-level
# "repositories" map of the rules_jvm_external lock.
pairs=$(awk '
  /^  "repositories": \{/ { in_map = 1; next }
  in_map && /^  \}/ { exit }
  in_map && /^    "https?:/ { gsub(/"/, "", $1); sub(/:$/, "", $1); repo = $1; next }
  in_map && /^      "/ { gsub(/[", ]/, ""); print repo, $0 }
' "$LOCK")

repos=$(cut -d' ' -f1 <<<"$pairs" | sort -u)
artifacts=$(cut -d' ' -f2 <<<"$pairs" | sort -u)
n_repos=$(grep -c . <<<"$repos")

failures=0
if [ -z "$pairs" ]; then
  echo "FAIL: no repositories map in $LOCK" >&2
  exit 1
fi
if [ "$n_repos" -lt 2 ]; then
  echo "FAIL: $LOCK pins against one host only: $repos" >&2
  failures=$((failures + 1))
fi
if ! grep -qvE '^https://(repo1\.maven\.org|repo\.maven\.apache\.org)/' <<<"$repos"; then
  echo "FAIL: every host is Maven Central's CDN, which fails as one: $(tr '\n' ' ' <<<"$repos")" >&2
  failures=$((failures + 1))
fi
while read -r repo; do
  while read -r artifact; do
    grep -qxF "$repo $artifact" <<<"$pairs" ||
      { echo "FAIL: $artifact has no URL on $repo" >&2; failures=$((failures + 1)); }
  done <<<"$artifacts"
done <<<"$repos"

[ "$failures" -eq 0 ] || exit 1
echo "PASS: $(grep -c . <<<"$artifacts") artifacts on $n_repos hosts"
