# JSONTestSuite corpus

The files under `test_parsing/` are the parsing test corpus from
[JSONTestSuite](https://github.com/nst/JSONTestSuite) by Nicolas Seriot —
the canonical "does your JSON parser agree with RFC 8259 and not crash on
hostile input" bank, described in
[Parsing JSON is a Minefield](https://seriot.ch/projects/parsing_json.html).

Vendored verbatim (MIT, see `LICENSE`) so `//runtime:json_conformance_test`
runs the whole corpus in CI. File naming is the suite's own convention:

- `y_*` — **must** be accepted (valid RFC 8259).
- `n_*` — **must** be rejected (invalid).
- `i_*` — **implementation-defined**; either outcome is conformant. We only
  require that these don't crash or hang.

The overriding invariant the test enforces for *every* file — `y_`, `n_`,
and `i_` alike — is that `opal::json::Decode` returns (an ok or an error),
never crashes. That is the property that caught the nesting-depth stack
overflow this suite was added alongside.

Known accepted `n_` case (documented in the test's allowlist rather than
"fixed"): `n_multidigit_number_then_00` is `123\0` — a valid number followed
by a NUL byte, which the nlohmann backend tolerates as trailing whitespace.
Harmless, and not worth diverging from the backend over.
