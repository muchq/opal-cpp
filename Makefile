# One-command verification (issue #48): `make verify` runs everything the CI
# gate runs, in one place, so full local verification stops being eight
# commands scattered across two build systems (docs/development.md#building-
# and-testing has the background). Each aggregate target is also callable on
# its own; the recipes deliberately mirror .github/workflows/ci.yml — when a
# CI job changes, change the matching target here.

BAZEL ?= bazelisk
GRADLE ?= gradle

# What CI gates a PR on: the bazel test matrix (one platform of it), the
# gradle build + format check, golden freshness, lockfile freshness, and the
# format/starlark lint.
.PHONY: verify
verify: test lockfiles codegen goldens lint
	@echo "verify: OK"

# verify plus the slower jobs: sanitizers, fuzzer smoke runs, the out-of-tree
# consumer module, and clang-tidy.
.PHONY: verify-full
verify-full: verify sanitize fuzz-smoke consumer tidy noexcept
	@echo "verify-full: OK"

# --config=werror mirrors the CI gate (build:ci implies it): first-party
# warnings are errors; external deps are exempt (see .bazelrc).
.PHONY: test
test:
	$(BAZEL) test //... --config=werror

.PHONY: codegen
codegen:
	cd codegen && $(GRADLE) build spotlessCheck

# The checked-in generated code is the golden output; regeneration must be
# byte-identical.
.PHONY: goldens
goldens:
	cd codegen && $(GRADLE) generateFixtures generateProtocolTests
	git diff --exit-code -- examples protocol-tests

.PHONY: lint
lint:
	find runtime examples codegen/compile-tests protocol-tests \
		\( -name '*.h' -o -name '*.cc' \) \
		! -path '*/generated/*' | xargs clang-format --dry-run --Werror
	buildifier --lint=warn --mode=check -r .

.PHONY: tidy
tidy:
	find runtime/src examples -name '*.cc' ! -name '*_test.cc' ! -path '*/src/json/*' \
		! -name 'beast_src.cc' ! -path '*/generated/*' -print0 \
		| xargs -0 -I{} clang-tidy --quiet {} -- -Iruntime/include -I. -std=c++20

.PHONY: sanitize
sanitize:
	CC=clang CXX=clang++ $(BAZEL) test //... --config=asan --config=ubsan --config=werror

# ADR-0003 enforcement gate: the dependency-light runtime must build with
# exceptions disabled (many large consumers compile -fno-exceptions).
# Boost.Asio/Beast are not -fno-exceptions-clean, so //runtime:http_beast and
# its dependents are excluded; every other runtime library is in.
.PHONY: noexcept
noexcept:
	$(BAZEL) build --config=noexcept --config=werror \
		//runtime:core //runtime:json //runtime:cbor //runtime:eventstream \
		//runtime:eventstream_json //runtime:eventstream_jsonrpc \
		//runtime:compression //runtime:http //runtime:server //runtime:client

.PHONY: fuzz-smoke
fuzz-smoke:
	set -e; for target in json_decode cbor_decode uri server_dispatch regex http1 access_log; do \
		echo "== fuzzing $$target"; \
		CC=clang CXX=clang++ $(BAZEL) build --config=fuzz "//fuzz:$${target}_fuzz"; \
		./bazel-bin/fuzz/$${target}_fuzz -max_total_time=30 -print_final_stats=1; \
	done

.PHONY: consumer
consumer:
	cd examples/bazel-consumer && $(BAZEL) test //... --config=werror \
		&& ./model-evolution-check.sh && ./boringssl-resolution-check.sh

# The checked-in MODULE.bazel.lock files must match what resolution would
# produce today; --lockfile_mode=error (the CI leg's mode) overrides the
# `off` that .bazelrc.user sets for the git-overrides flow.
.PHONY: lockfiles
lockfiles:
	$(BAZEL) mod deps --lockfile_mode=error
	cd examples/bazel-consumer && $(BAZEL) mod deps --lockfile_mode=error

# Line coverage for the runtime; the combined lcov report path prints at the
# end (render with genhtml, or read the CI job's artifact).
.PHONY: coverage
coverage:
	$(BAZEL) coverage //... --combined_report=lcov \
		--instrumentation_filter='//runtime[/:]'
	@echo "combined report: $$($(BAZEL) info output_path)/_coverage/_coverage_report.dat"

# Informational, never gates (PLAN Phase 7).
.PHONY: benchmarks
benchmarks:
	$(BAZEL) run -c opt //benchmarks:serde_benchmark -- --benchmark_min_time=0.2s
	$(BAZEL) run -c opt //benchmarks:request_benchmark -- --benchmark_min_time=0.2s
	$(BAZEL) run -c opt //benchmarks:beast_benchmark -- --benchmark_min_time=0.2s

# Rewrites instead of checking: the fix-it twin of `lint` + codegen's spotless.
.PHONY: format
format:
	find runtime examples codegen/compile-tests protocol-tests \
		\( -name '*.h' -o -name '*.cc' \) \
		! -path '*/generated/*' | xargs clang-format -i
	buildifier --lint=warn -r .
	cd codegen && $(GRADLE) spotlessApply
