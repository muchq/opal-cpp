# Development guide

Two build trees live in this repository (see PLAN §3.1):

| Tree | Language | Build | What it is |
|---|---|---|---|
| `runtime/` (+ future generated code, examples, integration tests) | C++20 | Bazel 9 | The `smithy-cpp-runtime` library that generated code links against |
| `codegen/` | Java 17 | Gradle | The Smithy → C++ generator (smithy-build plugin) |

## Prerequisites

- [bazelisk](https://github.com/bazelbuild/bazelisk) (reads `.bazelversion`, currently `9.x`)
- JDK 17+ and Gradle 9.6+ (or use `gradle/actions/setup-gradle` versions in CI)
- `clang-format` / `clang-tidy` (LLVM 18+) and `buildifier` for linting

## Building and testing

One command verifies everything the CI gate checks (bazel tests, gradle
build + format, golden freshness, lockfile freshness, lint):

```sh
make verify        # what CI gates a PR on
make verify-full   # + sanitizers, fuzzer smoke runs, the consumer module, clang-tidy
```

Each aggregate is also callable piecemeal (`make test lockfiles codegen
goldens lint sanitize fuzz-smoke consumer tidy coverage benchmarks format`);
the recipes mirror `.github/workflows/ci.yml`, one target per job. The
underlying commands:

```sh
# C++ runtime: build + run all tests, warnings-as-errors like CI
bazel test //... --config=werror

# With sanitizers (clang recommended: CC=clang CXX=clang++)
bazel test //... --config=asan --config=ubsan

# Module lockfiles: fail if a MODULE.bazel changed without its repin
# (run in the repo root and in examples/bazel-consumer)
bazel mod deps --lockfile_mode=error

# Codegen: build + unit tests + format check
cd codegen && gradle build spotlessCheck
```

Every first-party target compiles at `-Wall -Wextra` (`bazel/copts.bzl`;
the generated modules' BUILD files and the `bazel/defs.bzl` macros carry the
same flags). `--config=werror` — implied by CI's `--config=ci`, in the main
module and `examples/bazel-consumer` both — promotes those warnings to
errors for first-party code only, via a `--per_file_copt` label filter that
leaves external deps (boost, boringssl, zlib, ...) untouched, so warnings
gate every PR without a hard-coded `-Werror` ever reaching consumers
(issue #65).

The Java suite asserts on generated-source substrings; the compile-the-output
harness under `codegen/compile-tests/` is what proves hostile-but-legal models
(keyword member names, quote/backslash enum values, int64-extreme bounds —
issue #43's class) still *compile*: it runs the generator inside the Bazel
graph for every protocol, client and server mode both, and builds the result.
Extend its `model/gauntlet.smithy` when adding a new escaping/naming rule.

When a change touches what generated *headers* declare (operators, templates,
constraints), also build with `CC=clang CXX=clang++`: gcc accepts forms clang
rejects (e.g. deducing a defaulted `operator<=>` around a recursion cycle), and
the local default toolchain is gcc while half the CI matrix is clang.

Every header this repo ships — runtime and generated goldens alike — must be
**self-contained** (compile standalone): `REPO.bazel` enables the
`parse_headers` feature for this repo's packages, so ordinary `bazel build`
validates it here, in CI, and in consumers' builds. It is scoped via
REPO.bazel rather than `--features` deliberately: the global flag leaks into
third-party repos whose headers are not self-contained and never will be
(zlib's C headers, boost.context's pre-C++17 `std::result_of` polyfill —
the latter breaks consumers who run `--process_headers_in_dependencies`;
see the quickstart's header-validation section for the consumer-side story).

CI's `linux-llvm-libcxx` job runs the whole suite on the hermetic
`toolchains_llvm` toolchain (`--config=llvm`) — the only **linux libc++**
cell, and the toolchain serious Bazel consumers use. Do not use
`--config=llvm` behind a download-blocking proxy: it fetches an LLVM release
from GitHub (unmirrored); libc++-divergence checking stays a CI concern,
like the Beast targets. Note the
sweep's blind spot: local clang still uses libstdc++, while the macOS CI jobs
use libc++ — standard-library divergences (classically `vector<bool>`'s proxy
reference, whose libc++ form has no `std::hash`) only surface there, so
generic code should never assume container iteration yields real references.

## Benchmarks

```sh
bazel run -c opt //benchmarks:serde_benchmark      # Document pivot + JSON/CBOR codecs
bazel run -c opt //benchmarks:request_benchmark    # per-protocol client<->server round trips
bazel run -c opt //benchmarks:beast_benchmark      # real-TCP transports: socket, Beast, Beast TLS
```

CI runs all three as an informational job (no pass/fail threshold yet — the
numbers establish the baseline; see PLAN Phase 7).

## Formatting and linting

```sh
# C++
find runtime -name '*.h' -o -name '*.cc' | xargs clang-format -i

# Starlark (BUILD/MODULE/bzl)
buildifier --lint=warn -r .

# Java
cd codegen && gradle spotlessApply
```

## Repository conventions

- Runtime headers live under `runtime/include/opal/<module>/…` and are included as
  `#include "opal/<module>/….h"`.
- Every `cc_*` target loads rules from `@rules_cc//cc:defs.bzl` explicitly; the
  `--incompatible_autoload_externally` setting in `.bazelrc` exists only for third-party
  dependencies whose BUILD files predate Bazel 9's removal of the native C++ rules.
- Fixture Smithy models live under `examples/<name>/model/`; they double as test fixtures for
  codegen and the client↔server integration harness (PLAN Phases 2–5).
- Generated output under `examples/*/generated/` and `protocol-tests/*/generated/` is checked
  in as goldens: regenerate with `cd codegen && gradle generateFixtures generateProtocolTests`
  and commit model + output together — CI fails on drift. The drift check alone would let a
  generator bug ratify itself (both sides of the diff come from the same generator), so
  `GoldenProtocolTestAuditTest` additionally cross-checks the protocol-test goldens against the
  upstream suite definitions: every upstream case must be generated or excluded, wire facts must
  match, and exclusions must name real cases. The full loop (including the
  consumer-side story) is in [model-evolution.md](model-evolution.md).
- Goldens ratify generated-code *shape*, they don't judge it: redundant or dead emission
  compiles, passes conformance, and lives in the goldens until a human reads them (issue #68).
  Convention: every fix for a "generator emitted redundant/dead code" bug lands with an
  exactly-once or absence assertion — in `GeneratedCodeShapeTest`, or beside the feature's own
  tests like `HttpJsonBindingProtocolTest.serverWritesResponsePrefixHeadersExactlyOnce` — so the
  smell cannot quietly return. Placement rule for branch pins generally: a pin lands in the
  feature-owning test class when one exists (the compression-scoping pin lives in
  `BuildFileGeneratorTest`); `ConditionalWiringCoverageTest` is the fallback for arms with no
  owning class, not the default destination.
- **Sandboxed sessions (Claude Code on the web, and any proxy that allows
  git + GitHub release assets but blocks source archives):** run
  `bazel/make-git-overrides.sh`, then point `.bazelrc.user` at its output —
  after which the FULL `bazel test` surface works, Beast/openssl targets
  included, no exclusion list. This class of proxy admits git-over-HTTPS and
  `github.com/<org>/<repo>/releases/download/...` assets but 403s the
  on-demand archive endpoints (`/archive/refs/tags/...`,
  `codeload.github.com`) that most BCR modules (all of Boost, rules_perl via
  openssl's toolchains, google_benchmark) use as their source URL. The
  script finds every such module in `MODULE.bazel.lock` (plus the
  toolchain-registered stragglers the lockfile omits — see `EXTRA_MODULES`
  in the script), clones each at its pinned tag, replays the BCR's patches
  and overlay files and registry `MODULE.bazel` from `bcr.bazel.build`
  (registry metadata is not blocked), and writes one
  `--override_module` line per module into `<dest>/overrides.bazelrc`:

  ```sh
  bazel/make-git-overrides.sh            # builds ~/bazel-overrides (idempotent)
  cat >> .bazelrc.user <<'RC'
  common --lockfile_mode=off
  import /root/bazel-overrides/overrides.bazelrc
  RC
  ```

  Web sessions run this automatically: `.claude/hooks/session-start.sh`
  installs bazelisk (npm; the bazel binary itself is a release asset, which
  downloads fine), runs the script, and writes `.bazelrc.user`. Re-run the
  script after a dep bump; a 403 on a module it didn't cover means the
  module is toolchain-fetched and absent from the lockfile — add it to
  `EXTRA_MODULES`. Known residue: `libpfm` (a benchmark-only dep) downloads
  from SourceForge, which some proxies block — if `//benchmarks/...`
  fails to fetch, exclude it and rely on CI.
- Machine-specific Bazel flags go in `.bazelrc.user` (gitignored), e.g. a
  `--downloader_config` when working behind a proxy that blocks GitHub downloads. A rewrite of
  `github.com/(.*)` to `mirror.bazel.build/github.com/$1` unblocks most module archives; for the
  few that aren't mirrored (nlohmann_json, google_benchmark), `git clone` the exact tag (git often
  works where archive downloads don't) and point `--override_module=<name>=<checkout>` at it —
  nlohmann ships its own BUILD.bazel, so it only needs a MODULE.bazel in the checkout root (copy
  the patched one from the module's page on the Bazel Central Registry). Add
  `--lockfile_mode=off` so local runs don't dirty the checked-in MODULE.bazel.lock.
  Boost 1.90's asio also depends on the BCR `openssl` module, whose perl toolchain archives
  (`rules_perl`) are unmirrored too — and toolchain registration makes *every* analysis fetch
  them, not just openssl consumers. The Beast targets (the only openssl consumers) are already
  excluded behind a proxy, so point `--override_module=openssl=<dir>` at a stub directory
  holding an empty BUILD.bazel plus a MODULE.bazel containing only
  `module(name = "openssl", version = "3.5.4.bcr.1", compatibility_level = 3030100)`.
  The consumer-facing version of this recipe (including the `repo1.maven.org` fetch every
  git_override consumer triggers, and air-gapped prefetch) is in
  [quickstart.md](quickstart.md#the-first-build-cost-caching-and-locked-down-networks).
- The Boost-dependent targets (`//runtime:http_beast` and the tests that use it) fetch ~30
  modular Boost archives. Behind a proxy that blocks GitHub *entirely* they won't fetch
  (behind one that merely blocks archive endpoints, the git-override recipe above removes
  this whole exclusion list); exclude them and
  run everything else with
  `bazel test //... -- -//runtime:http_beast -//runtime:connection_event_recorder -//runtime:beast_transport_test -//runtime:beast_client_test -//runtime:beast_websocket_test -//examples/weather:weather_e2e_beast_test -//examples/simplerestjson:bookstore_server -//examples/simplerestjson:bookstore_server_lifecycle_test -//examples/chat/... -//examples/jsonrpc2/... -//protocol-tests/jsonrpc2:stream_conformance_test -//codegen/compile-tests:streaming_compile_test -//benchmarks/...`
  (the consumer module's `//:todo_beast_acceptance_test`, `//:websocket_acceptance_test`,
  `//:streaming_acceptance_test`, `//:jsonrpc_stream_cli_test`, and the
  `//:jsonrpc_stream_server` / `//:jsonrpc_stream_client` / `//:jsonrpc_raw_peer` binaries
  need the same exclusion). `//examples/chat/...` and `//examples/jsonrpc2/...` go
  wholesale because every generated *streaming* client library links the default Beast
  dialer's definition (ADR-0016) — even the in-memory `chat_e2e_test` and the fixture's
  generated tests are Beast targets at link time, wire or no wire; the jsonRpc2 stream
  conformance suite (ADR-0023) links its generated streaming client the same way.
  The Beast code itself can still be exercised against distro packages with plain g++ —
  `apt-get install libboost-dev libgtest-dev`, then:

  ```sh
  g++ -std=c++20 -O1 -pthread -Iruntime/include -Iruntime/testing/include \
    -I<nlohmann-json>/single_include \
    runtime/tests/http/beast_transport_test.cc \
    runtime/src/http/{beast_transport,socket_transport,headers,http1,server_dispatch,uri,trace_context}.cc \
    runtime/src/core/{uuid,text,base64,document_serde,regex,timestamp,version}.cc \
    runtime/src/eventstream/{frame,envelope,json_frame}.cc runtime/src/json/json.cc \
    -lgtest -lgtest_main -lssl -lcrypto -o /tmp/beast_test && /tmp/beast_test
  ```

  The same recipe runs `beast_client_test.cc` and `beast_websocket_test.cc` (swap the test
  file); the eventstream codec sources and the JSON stack ride along because the transport
  carries event-stream messages over WebSocket (ADR-0015) and negotiates the JSON-text
  wire for browser peers (ADR-0018) — nlohmann comes from `libnlohmann-json3-dev` (then
  drop the `-I`) or any checkout's `single_include/`.

  The chat example's e2e suites (Phase 8's exit criterion, CI-only under Bazel behind a
  proxy) run the same way with the generated sources and the JSON stack added — nlohmann
  comes from `libnlohmann-json3-dev` or any checkout's `single_include/`:

  ```sh
  g++ -std=c++20 -O1 -pthread -Iruntime/include -Iruntime/testing/include \
    -Iexamples/chat/generated/include -I<nlohmann-json>/single_include \
    examples/chat/chat_e2e_beast_test.cc examples/chat/generated/src/*.cc \
    runtime/src/core/{base64,document_serde,regex,text,timestamp,uuid,version}.cc \
    runtime/src/json/json.cc runtime/src/client/{retry,observability}.cc \
    runtime/src/http/{beast_transport,socket_transport,headers,http1,server_dispatch,uri,trace_context,websocket_pair,forwarded}.cc \
    runtime/src/eventstream/{frame,envelope,json_frame}.cc \
    runtime/src/server/{router,websocket_router,middleware,origin_gate}.cc \
    -lgtest -lgtest_main -lssl -lcrypto -o /tmp/chat_e2e && /tmp/chat_e2e
  ```

  Swap in `chat_e2e_test.cc` (or the fixture's generated `tests/*.cc`) for the in-memory
  suites — same source list; Beast is only a link-time passenger there. The browser-wire
  suite (`chat_browser_e2e_test.cc`, ADR-0018) runs with the same list too.

  Add `-g -fsanitize=thread` for a TSan pass — worth running on any change to the
  transport's threading (system Boost is header-only here, so no beast_src.cc and no
  Bazel involved). This catches the class of bug the proxy exclusions would otherwise
  defer to CI, e.g. an io_context going workless mid-request (PR #97). CI's tsan step
  runs the Beast suites too (alongside the registry and coroutine suites), so this
  recipe is the local pre-check, not the only line of defense.
