package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Protocol plug point (mirrors smithy-rs's protocol layer): owns the HTTP binding/dispatch code
 * emitted into generated clients, delegating document serde to the shared serde functions.
 */
interface ProtocolGenerator {

  /** e.g. "restJson1". */
  String name();

  /** The protocol trait's shape id (used to filter protocol test cases). */
  ShapeId traitId();

  /** Value for the Accept header and error-body content type. */
  String contentType();

  /** Extra runtime Bazel targets the generated client needs (":json" and/or ":cbor"). */
  List<String> runtimeDeps();

  /** Includes needed by client.cc beyond the shared set. */
  List<String> clientIncludes();

  /** Whether the protocol renames JSON body keys via @jsonName (restJson1 does). */
  default boolean usesJsonName() {
    return false;
  }

  /**
   * Whether an unidentified error response falls back to matching the operation's declared errors
   * by HTTP status (simpleRestJson: the X-Error-Type header is the discriminator, with status-code
   * fallback when absent). Only statuses unique within the operation's error set are matched.
   */
  default boolean errorStatusFallback() {
    return false;
  }

  /**
   * Emits the file-local helpers (error deserializer etc.) into client.cc's helpers namespace
   * (nested in the anonymous one, {@link ProtocolSupport#openHelpersNamespace}) — every emitted
   * call to one of them must be spelled {@code helpers::X}. Per-operation helpers stay out of the
   * serde functions' Serialize/Deserialize&lt;Shape&gt; naming pattern (Parse&lt;Op&gt;Error,
   * Build&lt;Op&gt;Response) so a shape named after an operation can never hide them via C++ name
   * hiding.
   */
  void writeClientHelpers(CppWriter w, CppContext context);

  /**
   * Emits statements patching protocol-specific bindings (e.g. simpleRestJson error @httpHeader
   * members) into the parsed error document ({@code parsed.doc}, guaranteed to be a map) before the
   * typed detail deserializes from it — so header-carried members satisfy @required. Runs inside
   * Make&lt;Error&gt;Error with {@code response} and {@code parsed} in scope. Default: nothing.
   */
  default void writeErrorDocPatches(
      CppWriter w, CppContext context, software.amazon.smithy.model.shapes.StructureShape error) {}

  /**
   * Whether a @streaming blob bound as the response @httpPayload streams to a caller-supplied
   * writer (issue #213): true for the HTTP binding protocols, where the payload is the whole
   * response body. The RPC protocols put every member inside one document, so there is no body to
   * hand over piecewise and their @streaming blobs stay buffered, as they always were.
   */
  default boolean supportsStreamingBlobPayloads() {
    return false;
  }

  /** Emits the body of one operation method (inside the function braces). */
  void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation);

  /**
   * Whether the protocol carries event-stream operations over WebSocket (ADR-0016). Every in-tree
   * protocol now does — the binding protocols on the framed envelope wire, jsonRpc2 on its native
   * wire (ADR-0023) — but a future protocol without a story gets a generation-time diagnostic
   * naming the operation instead of broken output.
   */
  default boolean supportsEventStreams() {
    return false;
  }

  /**
   * Whether streams speak the JSON-RPC-native wire (ADR-0023): text envelopes on the protocol's
   * shared endpoint — the opening request envelope selecting the operation and carrying
   * initial-request members in {@code params}, notification events, and a terminal response
   * envelope in place of the exception message. Drives the third initial-request validation mode,
   * the generated {@code JsonRpcStreamSocket} translation, and the {@code :eventstream_jsonrpc}
   * runtime dep.
   */
  default boolean streamsRideJsonRpcEnvelopes() {
    return false;
  }

  /**
   * Whether a streaming operation's initial-request members resolve through its @http
   * label/query/header bindings onto the upgrade request. False for fixed-upgrade-URI protocols
   * (rpcv2Cbor), where any initial-request member is rejected at generation time.
   */
  default boolean bindsInitialRequestMembers() {
    return false;
  }

  /** Expression producing an event payload (a opal::Blob) from a serialized Document. */
  default String eventPayloadEncode(String docExpr) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: " + name() + " does not support event streams");
  }

  /** Expression producing an Outcome&lt;opal::Document&gt; from an event payload Blob. */
  default String eventPayloadDecode(String payloadExpr) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: " + name() + " does not support event streams");
  }

  /**
   * Emits the body of one streaming operation method (ADR-0016): the upgrade target — the
   * operation's bindings for the binding protocols, the shared {@code /} endpoint for jsonRpc2
   * (ADR-0023, which also sends the opening request envelope before wrapping) — then the shared
   * dial-and-wrap tail ({@link EventStreamCodeGen#writeDialAndReturn}). Unreachable for protocols
   * that model no streams.
   */
  default void writeStreamingOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: " + name() + " does not support event streams");
  }

  /**
   * Emits the constructor statement registering one streaming operation's WebSocket route on {@code
   * stream_router_}. Unreachable for refusing protocols, like {@link #writeStreamingOperationBody};
   * single-endpoint protocols (jsonRpc2) override the plural {@link #writeStreamServerRoutes}
   * instead and never call this.
   */
  default void writeStreamServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: " + name() + " does not support event streams");
  }

  /**
   * Emits the constructor statement registering one streaming operation's shared-session route
   * (ADR-0021): an {@code AddSession} launch point that parses and refuses like {@link
   * #writeStreamServerRoute}, then hands the owned socket to the generated async wrapper.
   * Unreachable for refusing protocols; single-endpoint protocols override the plural {@link
   * #writeStreamSessionRoutes} instead and never call this.
   */
  default void writeStreamSessionRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: " + name() + " does not support event streams");
  }

  /**
   * Emits the constructor statements registering the service's streaming routes. The default
   * registers one route per operation via {@link #writeStreamServerRoute}; jsonRpc2 overrides this
   * with one shared-endpoint route that dispatches on the opening envelope's {@code method}
   * (ADR-0023) — the {@link #writeServerRoutes} move, transposed to streams.
   */
  default void writeStreamServerRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    for (OperationShape operation : operations) {
      writeStreamServerRoute(w, context, service, operation);
    }
  }

  /** The shared-session sibling of {@link #writeStreamServerRoutes}, same override rule. */
  default void writeStreamSessionRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    for (OperationShape operation : operations) {
      writeStreamSessionRoute(w, context, service, operation);
    }
  }

  /** Includes server.cc needs beyond the shared set. */
  List<String> serverIncludes();

  /** Emits server-side file-local helpers (error mapping, per-op parse/serialize functions). */
  void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations);

  /**
   * Emits the constructor statements registering the service's routes. The default registers one
   * route per operation via {@link #writeServerRoute}; single-endpoint protocols (jsonRpc2, which
   * dispatches on the request body's {@code method} member) override this instead.
   */
  default void writeServerRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    for (OperationShape operation : operations) {
      writeServerRoute(w, context, service, operation);
    }
  }

  /** Emits the constructor statements registering one operation's route. */
  default void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    throw new software.amazon.smithy.codegen.core.CodegenException(
        "cpp-codegen: "
            + name()
            + " does not emit per-operation routes; single-endpoint protocols override"
            + " writeServerRoutes instead");
  }
}
