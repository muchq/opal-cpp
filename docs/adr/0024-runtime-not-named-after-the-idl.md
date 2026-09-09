# ADR-0024: The runtime is not named after the IDL

**Status:** Accepted (2026-09-08). Issue #201. Implemented in three PRs,
one per surface, in this order: the C++ namespace (this ADR lands with
it), the include root, the Bazel module and labels.

## Context

Smithy is the IDL a service is *described* in. It is not a property of
the service, of an HTTP request, of a JSON encoder, or of a TLS socket.
Yet every runtime type carried the IDL's name: `smithy::Outcome`,
`smithy::json::Encode`, `smithy::http::BeastServerTransport`. A service
that never opens a model still wrote `smithy::` for its JSON codec and
its HTTP client, and a fleet running services from several toolchains
had one of them announcing its build system in every type name.

The first symptom to hurt was wire-visible: the built-in metric families
shipped as `smithy_http_requests_total`, which a dashboard has no reason
to know and which the fleet contract (#199) does not spell that way. That
was fixed in place. Issue #201 is the rest of the audit, and it is a
decision rather than a task: a rename this wide is a breaking change
with a real migration cost, and the honest options were to leave the
wart, rename behind a compatibility alias, or rename outright.

`docs/versioning.md` allows breaking changes on a minor bump pre-1.0
when they are called out under a "Breaking" heading with a migration
note. There is one out-of-tree consumer in the repository
(`examples/bazel-consumer`) and the known external consumers are
in-house. A compatibility alias would have carried the old spelling
through a deprecation window nobody asked for, doubled every header, and
put off the same diff to a release with more consumers rather than fewer.

## Decision

Rename outright, without an alias, on the next minor release. The runtime
is `opal`:

| Surface | Before | After |
| --- | --- | --- |
| C++ namespace | `smithy::`, `smithy::http::`, … | `opal::`, `opal::http::`, … |
| Include root | `#include "smithy/http/transport.h"` | `#include "opal/http/transport.h"` |
| Bazel module and labels | `bazel_dep(name = "smithy_cpp")`, `@smithy_cpp//runtime:http` | `bazel_dep(name = "opal_cpp")`, `@opal_cpp//runtime:http` |

The distinction the audit applies: a name that refers to the **model**
keeps `smithy`; a name that refers to a **runtime thing that would exist
identically had the service been hand-written** does not. So these stay:

- The repository, `smithy-cpp`. It is a Smithy tool.
- The codegen plugin, `codegen/`, `io.smithycpp.codegen`, and the
  `software.amazon.smithy` dependency.
- The Bazel rules `smithy_cpp_{types,client,server}_library`. They take a
  `.smithy` model as input.
- Smithy namespaces in models (`smithy.cpp.protocols#jsonRpc2`,
  `smithy.api#…`) and the C++ namespaces a model's author derives from
  them — the rules-test fixture's `smithy::cpp::ruletest` mirrors its
  model's `smithy.cpp.ruletest` and is the consumer's, not the runtime's.
- Prose about "the Smithy operation", traits, the model. The `route`
  label really is the Smithy operation name.

And these change, each as its own PR because a mixed one is unreviewable:

1. **Namespace.** Every `smithy::` scope in hand-written code, in the 26
   codegen emitters that print it into generated code, and in docs. The
   generated protocol-conformance suites move with it
   (`opal::protocoltests::…`), because they are generated code that
   refers to the runtime, not a model. Goldens regenerate from the
   emitters; nothing under `generated/` is hand-edited.
2. **Include root.** `runtime/include/smithy/` becomes
   `runtime/include/opal/`, with the include guards and the
   `SMITHY_*` macro prefix that derive from it, and the `smithy:` prefix
   on the runtime's own log lines, which names the library that wrote
   them.
3. **Bazel.** The module name, the `@smithy_cpp//…` labels the consumer
   module and the docs use, and `SMITHY_COPTS`.

Sequencing constraint, the same for every surface: the codegen emitters
are the source of truth for generated references, so they change first
and the goldens regenerate from them.

## Consequences

- Breaking, on a minor bump, called out in the CHANGELOG with the
  one-line migration each surface needs (a `sed` over the consumer's
  tree). Consumers migrate once per surface, or once at the release that
  carries all three.
- The rename is mechanical and verified by the existing suites: the
  goldens must regenerate byte-identically from the emitters (the codegen
  job's golden check), and every in-tree consumer — the examples, the
  protocol conformance suites, the out-of-tree module — compiles against
  the new name with no source change beyond the rename itself.
- Wire-visible strings (metric names, log-line keys, headers) never carry
  the IDL's name; the audit that produced this ADR checked them and the
  metrics fix in #199 was the only instance.
- `docs/versioning.md`'s compatibility surface #3 reads
  `runtime/include/smithy/**` until the include-root PR moves it.
