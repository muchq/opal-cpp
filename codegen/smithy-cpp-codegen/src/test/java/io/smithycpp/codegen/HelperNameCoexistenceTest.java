package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;

/**
 * Pins the two structural answers to C++ name hiding between model types and generated file-local
 * helpers. Issue #64's renames keep per-operation helpers out of the serde functions'
 * Serialize/Deserialize&lt;Shape&gt; naming pattern (Parse&lt;Op&gt;Error,
 * Build&lt;Op&gt;Response), so a shape named after an operation coexists with the helpers — models
 * #69's guard used to reject now generate. Issue #71's helpers namespace covers the complementary
 * direction those renames can't: every helper lives in {@code helpers::} (nested in the anonymous
 * namespace) and helper code references model types as {@code types::X}, so a shape named exactly
 * like a helper coexists too — the compile gauntlet's Shadow operation compiles that closure for
 * every protocol, and {@link #shapeNamedAfterAHelperCoexistsWithTheHelper} pins the generation
 * shape here. Each #64 test uses the model shape that actually materialized the hiding: a
 * same-named serde call inside the file that declares the helper.
 */
class HelperNameCoexistenceTest {

  @Test
  void errorShapeNamedAfterTheOperationCoexistsWithTheErrorHelper() {
    // Make<Shape>Error deserializes the error detail INSIDE client.cc, so an
    // error shape named GetError calls serde's DeserializeGetError(Document)
    // in the same file that declares the per-operation error helper. With the
    // helper named DeserializeGetError(HttpResponse) that call was hidden;
    // as ParseGetError the two coexist.
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { name: String }
            errors: [GetError]
        }

        @error("client")
        structure GetError {
            message: String
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String client = manifest.expectFileString("/src/client.cc");
    assertTrue(
        client.contains("opal::Error ParseGetError(const opal::http::HttpResponse&"), client);
    assertTrue(client.contains("DeserializeGetError(parsed.doc)"), client);
  }

  @Test
  void outputMemberTypeNamedAfterTheOperationCoexistsWithTheResponseHelper() {
    // Build<Op>Response serializes the output body INSIDE server.cc, so an
    // output member of type GetResponse calls serde's SerializeGetResponse in
    // the same file. A helper named SerializeGetResponse hid that call from
    // within its own body; as BuildGetResponse the two coexist.
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Get] }
        @http(method: "POST", uri: "/get")
        operation Get {
            input := { name: String }
            output := { payload: GetResponse }
        }

        structure GetResponse {
            message: String
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains("opal::http::HttpResponse BuildGetResponse("), server);
    // The full call expression, not just the name — a regressed helper
    // declaration would also contain "SerializeGetResponse(".
    assertTrue(server.contains("SerializeGetResponse((*output.payload))"), server);
  }

  @Test
  void shapeNamedAfterAHelperCoexistsWithTheHelper() {
    // GenericError is a fixed client.cc helper AND, here, a modeled error
    // shape. The helpers namespace keeps both alive: the helper is declared
    // inside helpers::, calls to it are helpers::-qualified, and the type is
    // referenced as types::GenericError where helper code needs it.
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get {
            input := { name: String }
            errors: [GenericError]
        }

        @error("client")
        structure GenericError {
            message: String
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String client = manifest.expectFileString("/src/client.cc");
    assertTrue(client.contains("namespace helpers {"), client);
    // The helper's fallback return and the same-named error's factory both
    // resolve through qualified calls.
    assertTrue(client.contains("return helpers::GenericError(std::move(parsed));"), client);
    assertTrue(
        client.contains(
            "if (parsed.code == \"GenericError\") return helpers::MakeGenericErrorError("),
        client);
    // The mirror direction: helper code references the TYPE through the types
    // alias, which only files that need it carry.
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(server.contains("namespace types = ::test::coexist;"), server);
    assertTrue(server.contains("error.detail<types::GenericError>()"), server);
  }

  @Test
  void errorLessOperationsSkipTheErrorParserAndFallBackToGenericError() {
    // Parse<Op>Error only exists for operations that declare errors; an
    // error-less operation's body returns GenericError(ParseError(...))
    // directly (previously pinned by a guard-scoping test, now by output).
    String model =
        """
        $version: "2.0"
        namespace test.coexist
        use smithy.cpp.protocols#jsonRpc2

        @jsonRpc2
        service Svc { version: "1", operations: [Get] }
        operation Get { input := { name: String } }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.coexist#Svc", "test::coexist");
    String client = manifest.expectFileString("/src/client.cc");
    assertFalse(client.contains("ParseGetError"), client);
    assertTrue(client.contains("helpers::GenericError(helpers::ParseError(*response))"), client);
  }
}
