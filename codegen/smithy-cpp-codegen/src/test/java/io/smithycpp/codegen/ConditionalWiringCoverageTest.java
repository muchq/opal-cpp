package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.model.validation.ValidatedResultException;

/**
 * Branch pins for conditional emissions no fixture exercises (issue #68 item 3): the inventory of
 * service/operation-gated generation found these arms living outside every compiled golden, so a
 * regression would surface only in a downstream consumer. Each test pins one arm textually via
 * {@link PluginTestHarness}; the compiled-fixture arms live in the roundtrip model.
 */
class ConditionalWiringCoverageTest {

  @Test
  void rpcv2CborWithoutConstraintsEmitsNoValidationMachinery() {
    // The validation wiring's absent branch for rpcv2Cbor: every compiled
    // cbor fixture has constraints, so the definition/call pairing of
    // ValidationErrorResponse and AddValidationFailure was only ever seen
    // emitted. A constraint-free service must contain neither.
    String model =
        """
        $version: "2.0"
        namespace test.wiring
        use smithy.protocols#rpcv2Cbor

        @rpcv2Cbor
        service Svc { version: "1", operations: [Op] }
        operation Op { input := { name: String } }
        """;
    String server =
        PluginTestHarness.generate(model, "test.wiring#Svc", "test::wiring")
            .expectFileString("/src/server.cc");
    assertFalse(server.contains("ValidationErrorResponse"), server);
    assertFalse(server.contains("AddValidationFailure"), server);
  }

  @Test
  void apiKeyHeaderSchemePrefixesTheHeaderValue() {
    // @httpApiKeyAuth's scheme arm: cafe covers header keys without a
    // scheme, roundtrip covers query keys; the "Scheme <key>" form had no
    // coverage at all.
    String model =
        """
        $version: "2.0"
        namespace test.wiring
        use alloy#simpleRestJson

        @simpleRestJson
        @httpApiKeyAuth(name: "authorization", in: "header", scheme: "ApiKey")
        service Svc { version: "1", operations: [Op] }
        @http(method: "POST", uri: "/op")
        operation Op { input := { name: String } }
        """;
    String client =
        PluginTestHarness.generate(model, "test.wiring#Svc", "test::wiring")
            .expectFileString("/src/client.cc");
    assertTrue(
        client.contains("request.headers.Set(\"authorization\", \"ApiKey \" + config_.api_key());"),
        client);
  }

  @Test
  void requiredIdempotencyTokensAutofillOnEmptyNotUnset() {
    // prepareIdempotencyTokens' @required arm (.empty(), not has_value()),
    // exercised through the HTTP+JSON call site — both were uncovered: cafe
    // and the calculator cover only optional tokens on the RPC protocols.
    String model =
        """
        $version: "2.0"
        namespace test.wiring
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Op] }
        @http(method: "POST", uri: "/op")
        operation Op {
            input := {
                @required
                @idempotencyToken
                token: String
            }
        }
        """;
    String client =
        PluginTestHarness.generate(model, "test.wiring#Svc", "test::wiring")
            .expectFileString("/src/client.cc");
    assertTrue(
        client.contains("if (prepared.token.empty()) prepared.token = opal::GenerateUuidV4();"),
        client);
  }

  @Test
  void paginatedOutputTokensCannotBeRequired() {
    // Chasing the pager's former required-token arm found it unreachable:
    // Smithy's paginated validator rejects @required output tokens at
    // assembly. This pins the assumption the generator now leans on (the
    // pager emits only the optional-token forms); if a future Smithy version
    // ever allows required tokens, this fails and the arm must come back.
    String model =
        """
        $version: "2.0"
        namespace test.wiring
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Op] }
        @paginated(inputToken: "next", outputToken: "next")
        @readonly
        @http(method: "GET", uri: "/op")
        operation Op {
            input := {
                @httpQuery("next")
                next: String
            }
            output := {
                @required
                next: String
            }
        }
        """;
    var error =
        assertThrows(
            ValidatedResultException.class,
            () -> PluginTestHarness.generate(model, "test.wiring#Svc", "test::wiring"));
    assertTrue(error.getMessage().contains("outputToken"), error.getMessage());
  }

  @Test
  void malformedTestsOffSuppressesTheFileAndItsBuildTargetTogether() {
    // hasMalformedTests is computed once and threaded to both the test file
    // and the tests BUILD target — this pins the pairing in the OFF state
    // (the ON state is every protocol-tests module).
    String model =
        """
        $version: "2.0"
        namespace test.wiring
        use smithy.protocols#rpcv2Cbor
        use smithy.test#httpMalformedRequestTests

        @rpcv2Cbor
        service Svc { version: "1", operations: [Op] }
        operation Op { input := { name: String } }

        apply Op @httpMalformedRequestTests([
            {
                id: "Garbage"
                protocol: rpcv2Cbor
                request: { method: "POST", uri: "/service/Svc/operation/Op", body: "xx" }
                response: { code: 400 }
            }
        ])
        """;
    MockManifest manifest =
        PluginTestHarness.generate(
            model,
            "test.wiring#Svc",
            "test::wiring",
            settings ->
                settings
                    .withMember("testsPackage", "//test/wiring")
                    .withMember("malformedTests", false));
    assertFalse(
        manifest.hasFile("/tests/server_malformed_tests.cc"), manifest.getFiles().toString());
    String testsBuild = manifest.expectFileString("/tests/BUILD.bazel");
    assertFalse(testsBuild.contains("server_malformed"), testsBuild);
  }
}
