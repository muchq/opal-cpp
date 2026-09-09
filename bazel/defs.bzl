"""Consumer-facing rules: generate Smithy C++ clients/servers inside the build graph.

Usage (from any Bazel 8/9 module that depends on opal_cpp):

    load("@opal_cpp//bazel:defs.bzl", "smithy_cpp_client_library", "smithy_cpp_server_library")

    smithy_cpp_client_library(
        name = "weather_client",
        srcs = ["model/weather.smithy"],
        namespace = "acme::weather",
        service = "acme.weather#Weather",
    )

Each macro runs the generator as a hermetic Java action (no Gradle, no scripts)
and wraps the generated sources in an ordinary cc_library that consumers depend
on like any other target. The service's protocol comes from the model itself
(alloy#simpleRestJson, smithy.protocols#rpcv2Cbor, or smithy.cpp.protocols#jsonRpc2).
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel/private:validation.bzl", "namespace_error", "service_error")

_GENERATOR = Label("//codegen:generator")
_RUNTIME_DEPS = [
    Label("//runtime:cbor"),
    Label("//runtime:client"),
    Label("//runtime:compression"),
    Label("//runtime:core"),
    Label("//runtime:eventstream"),
    Label("//runtime:eventstream_jsonrpc"),
    Label("//runtime:http"),
    Label("//runtime:json"),
]
_SERVER_RUNTIME_DEPS = [Label("//runtime:server")]

def _generated_files(namespace, mode):
    """The exact files the generator emits for a mode (namespace drives paths)."""
    ns_path = namespace.replace("::", "/")
    hdrs = ["include/%s/types.h" % ns_path]
    srcs = []
    if mode != "types":
        hdrs.append("include/%s/serde.h" % ns_path)
        srcs.append("src/serde.cc")
    if mode in ("client", "both"):
        hdrs.append("include/%s/client.h" % ns_path)
        srcs.append("src/client.cc")
    if mode in ("server", "both"):
        hdrs.append("include/%s/server.h" % ns_path)
        srcs.append("src/server.cc")
    return hdrs, srcs

def _smithy_cpp_generate_impl(ctx):
    # Wiring mistakes fail here, at analysis time, with the fix in the message —
    # the generator never launches (docs/quickstart.md#troubleshooting-generation-failures).
    err = namespace_error(ctx.attr.namespace) or service_error(ctx.attr.service)
    if err:
        fail(err)

    hdr_paths, src_paths = _generated_files(ctx.attr.namespace, ctx.attr.mode)
    hdr_outputs = [ctx.actions.declare_file(ctx.label.name + "/" + p) for p in hdr_paths]
    src_outputs = [ctx.actions.declare_file(ctx.label.name + "/" + p) for p in src_paths]
    outputs = hdr_outputs + src_outputs

    # All outputs share the root <bindir>/<package>/<name>.
    first = outputs[0]
    out_root = first.path[:-(len(hdr_paths[0]) + 1)]

    args = ctx.actions.args()
    for model in ctx.files.srcs:
        args.add("--model", model.path)
    args.add("--service", ctx.attr.service)
    args.add("--namespace", ctx.attr.namespace)
    args.add("--mode", ctx.attr.mode)
    args.add("--emit-build-file", "false")
    args.add("--output", out_root)

    ctx.actions.run(
        executable = ctx.executable._generator,
        arguments = [args],
        inputs = ctx.files.srcs,
        outputs = outputs,
        mnemonic = "SmithyCppGenerate",
        progress_message = "Generating Smithy C++ (%s) for %s" % (ctx.attr.mode, ctx.attr.service),
        toolchain = None,
    )
    return [
        DefaultInfo(files = depset(outputs)),
        OutputGroupInfo(hdrs = depset(hdr_outputs), srcs = depset(src_outputs)),
    ]

_smithy_cpp_generate = rule(
    implementation = _smithy_cpp_generate_impl,
    attrs = {
        "mode": attr.string(values = ["types", "client", "server", "both"], mandatory = True),
        "namespace": attr.string(mandatory = True),
        "service": attr.string(mandatory = True),
        "srcs": attr.label_list(
            allow_empty = False,
            allow_files = [".smithy", ".json"],
            mandatory = True,
        ),
        "_generator": attr.label(
            default = _GENERATOR,
            executable = True,
            cfg = "exec",
        ),
    },
)

def _smithy_cpp_library(name, srcs, service, namespace, mode, deps, **kwargs):
    gen = name + "_smithy_gen"

    # Caller-supplied deps are appended to the runtime set — the seam for
    # deps generation alone cannot decide, e.g. //runtime:http_beast on a
    # streaming client whose binaries link the default Beast dialer
    # (ADR-0016; the generated-BUILD path adds it automatically).
    extra_deps = kwargs.pop("deps", [])

    # Generated code compiles at the runtime's own warning level everywhere it
    # is consumed, so a generator regression that starts emitting warnable code
    # surfaces in any -Werror build (issue #65; the checked-in golden modules'
    # BUILD files carry the same flags). Caller copts append, so a consumer can
    # add but not silently drop the baseline.
    copts = ["-Wall", "-Wextra"] + kwargs.pop("copts", [])

    # tags/testonly apply to the internal targets too, so e.g. tags = ["manual"]
    # keeps every piece of the macro out of wildcard builds.
    inherited = {k: kwargs[k] for k in ("tags", "testonly") if k in kwargs}
    _smithy_cpp_generate(
        name = gen,
        srcs = srcs,
        service = service,
        namespace = namespace,
        mode = mode,
        visibility = ["//visibility:private"],
        **inherited
    )
    native.filegroup(
        name = gen + "_hdrs",
        srcs = [":" + gen],
        output_group = "hdrs",
        visibility = ["//visibility:private"],
        **inherited
    )
    native.filegroup(
        name = gen + "_srcs",
        srcs = [":" + gen],
        output_group = "srcs",
        visibility = ["//visibility:private"],
        **inherited
    )
    cc_library(
        name = name,
        hdrs = [":" + gen + "_hdrs"],
        srcs = [":" + gen + "_srcs"] if mode != "types" else [],
        copts = copts,
        includes = [gen + "/include"],
        deps = [str(d) for d in deps] + extra_deps,
        **kwargs
    )

def smithy_cpp_types_library(name, srcs, service, namespace, **kwargs):
    """Generated data types only (types.h)."""
    _smithy_cpp_library(
        name,
        srcs,
        service,
        namespace,
        "types",
        [Label("//runtime:core")],
        **kwargs
    )

def smithy_cpp_client_library(name, srcs, service, namespace, **kwargs):
    """Generated <Service>Client (+types/serde) as an ordinary cc_library."""
    _smithy_cpp_library(name, srcs, service, namespace, "client", _RUNTIME_DEPS, **kwargs)

def smithy_cpp_server_library(name, srcs, service, namespace, **kwargs):
    """Generated <Service>Handler/<Service>Server (+types/serde) as an ordinary cc_library."""
    _smithy_cpp_library(
        name,
        srcs,
        service,
        namespace,
        "server",
        _RUNTIME_DEPS + _SERVER_RUNTIME_DEPS,
        **kwargs
    )
