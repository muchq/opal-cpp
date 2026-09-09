package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;

/**
 * ADR-0010 pins: every protocol's generated handler interface takes the request context, and every
 * dispatch path hands it to the handler call — so no protocol can silently regress to dropping the
 * metadata one call before the handler (issue #46).
 */
class HandlerContextThreadingTest {

  private static final String CONTEXT_PARAM = "const opal::server::RequestContext& context";

  private static final String REST_MODEL =
      """
      $version: "2.0"
      namespace test.ctx
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping { input := { name: String }, output := { reply: String } }
      """;

  private static final String CBOR_MODEL =
      """
      $version: "2.0"
      namespace test.ctx
      use smithy.protocols#rpcv2Cbor

      @rpcv2Cbor
      service Svc { version: "1", operations: [Ping] }

      operation Ping { input := { name: String }, output := { reply: String } }
      """;

  private static final String JSONRPC_MODEL =
      """
      $version: "2.0"
      namespace test.ctx
      use smithy.cpp.protocols#jsonRpc2

      @jsonRpc2
      service Svc { version: "1", operations: [Ping] }

      operation Ping { input := { name: String }, output := { reply: String } }
      """;

  @Test
  void restInterfaceAndDispatchThreadTheContext() {
    MockManifest manifest = PluginTestHarness.generate(REST_MODEL, "test.ctx#Svc", "test::ctx");
    String header = manifest.expectFileString("/include/test/ctx/server.h");
    assertTrue(header.contains("Ping(const PingInput& input, " + CONTEXT_PARAM + ") = 0;"), header);
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains("handler->Ping(*input, context)"), server);
  }

  @Test
  void cborRouteLambdaNamesAndForwardsTheContext() {
    MockManifest manifest = PluginTestHarness.generate(CBOR_MODEL, "test.ctx#Svc", "test::ctx");
    String header = manifest.expectFileString("/include/test/ctx/server.h");
    assertTrue(header.contains("Ping(const PingInput& input, " + CONTEXT_PARAM + ") = 0;"), header);
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains(CONTEXT_PARAM + ") -> opal::http::HttpResponse {"), server);
    assertTrue(server.contains("handler->Ping(input, context)"), server);
  }

  @Test
  void jsonRpcHandleFunctionsThreadTheContext() {
    MockManifest manifest = PluginTestHarness.generate(JSONRPC_MODEL, "test.ctx#Svc", "test::ctx");
    String header = manifest.expectFileString("/include/test/ctx/server.h");
    assertTrue(header.contains("Ping(const PingInput& input, " + CONTEXT_PARAM + ") = 0;"), header);
    String server = manifest.expectFileString("/src/server.cc");
    // The envelope dispatch happens in Handle<Op> free functions: the context
    // enters their signature and rides every dispatch call.
    assertTrue(server.contains("const opal::Document& id, " + CONTEXT_PARAM + ") {"), server);
    assertTrue(server.contains("handler.Ping(input, context)"), server);
    assertTrue(server.contains("HandlePing(*handler, *params, id, context)"), server);
  }
}
