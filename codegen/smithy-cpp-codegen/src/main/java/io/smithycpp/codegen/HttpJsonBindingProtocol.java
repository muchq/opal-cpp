package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * Shared HTTP + JSON binding protocol: HTTP method/URI from @http, labels, query, headers, and JSON
 * request/response bodies. This class is only the {@link ProtocolGenerator} facade — the emission
 * lives in {@link HttpJsonClientGenerator} (operation-method bodies), {@link
 * HttpJsonServerGenerator} (parse/serialize functions, routes, content negotiation), and {@link
 * HttpBindingCodeGen} (the read/write binding emitters both halves share).
 *
 * <p>A concrete protocol is one {@link #simpleRestJson()}-style factory supplying the protocol
 * identity: name, trait id, error-identity header, and status-fallback behavior.
 */
final class HttpJsonBindingProtocol implements ProtocolGenerator {

  static final ShapeId SIMPLE_REST_JSON_TRAIT = ShapeId.from("alloy#simpleRestJson");

  /**
   * alloy#simpleRestJson: the vendor-neutral HTTP + JSON protocol used by smithy4s. Adopted from
   * alloy-core; the recommended REST protocol for 0.1.0. It carries the entire HTTP-binding surface
   * of this generator; error identity is the neutral {@code X-Error-Type} response header (the
   * error shape name), with status-code fallback, rather than a vendor header.
   */
  static HttpJsonBindingProtocol simpleRestJson() {
    return new HttpJsonBindingProtocol(
        "simpleRestJson", SIMPLE_REST_JSON_TRAIT, "x-error-type", /* errorStatusFallback= */ true);
  }

  private final String name;
  private final ShapeId traitId;
  private final boolean errorStatusFallback;
  private final HttpJsonClientGenerator client;
  private final HttpJsonServerGenerator server;

  private HttpJsonBindingProtocol(
      String name, ShapeId traitId, String errorTypeHeaderName, boolean errorStatusFallback) {
    this.name = name;
    this.traitId = traitId;
    this.errorStatusFallback = errorStatusFallback;
    // The halves mirror usesJsonName() so binding-emitted body keys can never
    // disagree with the serde functions, which get the flag via DirectedCppCodegen.
    this.client = new HttpJsonClientGenerator(errorTypeHeaderName, usesJsonName());
    this.server = new HttpJsonServerGenerator(errorTypeHeaderName, usesJsonName());
  }

  @Override
  public String name() {
    return name;
  }

  @Override
  public ShapeId traitId() {
    return traitId;
  }

  @Override
  public String contentType() {
    return "application/json";
  }

  @Override
  public boolean usesJsonName() {
    return true;
  }

  @Override
  public boolean errorStatusFallback() {
    return errorStatusFallback;
  }

  @Override
  public List<String> runtimeDeps() {
    return List.of(":json");
  }

  @Override
  public List<String> clientIncludes() {
    return client.includes();
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    client.writeHelpers(w);
  }

  @Override
  public void writeErrorDocPatches(CppWriter w, CppContext context, StructureShape error) {
    client.writeErrorDocPatches(w, context, error);
  }

  @Override
  public void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    client.writeOperationBody(w, context, service, operation);
  }

  @Override
  public List<String> serverIncludes() {
    return server.includes();
  }

  @Override
  public void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    server.writeHelpers(w, context, service, operations);
  }

  @Override
  public void writeServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    server.writeRoute(w, context, service, operation);
  }

  @Override
  public boolean supportsEventStreams() {
    return true;
  }

  @Override
  public boolean bindsInitialRequestMembers() {
    return true;
  }

  @Override
  public String eventPayloadEncode(String docExpr) {
    return "opal::Blob::FromString(opal::json::Encode(" + docExpr + "))";
  }

  @Override
  public String eventPayloadDecode(String payloadExpr) {
    return "opal::json::Decode(" + payloadExpr + ".ToString())";
  }

  @Override
  public void writeStreamingOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    client.writeStreamingOperationBody(w, context, service, operation);
  }

  @Override
  public void writeStreamServerRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    server.writeStreamRoute(w, context, service, operation);
  }

  @Override
  public void writeStreamSessionRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    server.writeStreamSessionRoute(w, context, service, operation);
  }
}
