package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

/**
 * Exactly-once / absence pins for "generator emitted redundant/dead code" fixes with no
 * feature-owning test class — see the convention in docs/development.md (issue #68).
 */
class GeneratedCodeShapeTest {

  private static final String NO_INPUT_MODEL_TEMPLATE =
      """
      $version: "2.0"
      namespace test.shape
      use %s

      @%s
      service Svc { version: "1", operations: [Ping] }
      operation Ping {}
      """;

  private static final String UNION_MODEL =
      """
      $version: "2.0"
      namespace test.shape
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping { input := { status: Status } }

      union Status { pending: Pending, ready: Ready }
      structure Pending { position: Integer }
      structure Ready { at: Timestamp }
      """;

  @Test
  void unionAccessorsFailWithContextAndOfferSafeAlternatives() {
    // Issue #49: wrong-case as_x() must die naming the union, the requested
    // member, and the engaged member — never throw a context-free
    // std::bad_variant_access — and consumers get a pointer-returning
    // accessor, case_name(), and visit() for checked access.
    String types =
        PluginTestHarness.generate(UNION_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include \"opal/core/fatal.h\""), types);
    assertTrue(types.contains("require_is(1, \"pending\");"), types);
    assertTrue(
        types.contains(
            "opal::internal::FatalWrongUnionAccess(\"Status\", requested," + " case_name());"),
        types);
    assertTrue(
        types.contains(
            "const Pending* as_pending_or_null() const" + " { return std::get_if<1>(&value_); }"),
        types);
    assertTrue(
        types.contains(
            "static constexpr const char* kNames[] =" + " {\"(empty)\", \"pending\", \"ready\"};"),
        types);
    assertTrue(types.contains("decltype(auto) visit(Visitor&& visitor) const"), types);
  }

  private static final String ERRORS_MODEL =
      """
      $version: "2.0"
      namespace test.shape
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping {
          input := { @required id: String }
          errors: [NotFound, Quota]
      }

      @error("client")
      @httpError(404)
      structure NotFound { message: String }

      @error("client")
      @httpError(429)
      structure Quota { message: String }
      """;

  @Test
  void operationsGrowTypedErrorListings() {
    // Issue #49: modeled-error dispatch was stringly-typed — consumers
    // compared Error::code() text and guessed the detail<T>() type. Every
    // operation with modeled errors now gets a <Op>Errors listing in
    // client.h: FromError() matches kind + code and carries the typed
    // detail, and the union accessor surface (is_/as_/or_null/case_name/
    // visit) makes dispatch exhaustive and typo-proof.
    String client =
        PluginTestHarness.generate(ERRORS_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/client.h");
    assertTrue(client.contains("class PingErrors {"), client);
    assertTrue(client.contains("static PingErrors FromError(const opal::Error& error) {"), client);
    assertTrue(
        client.contains("if (error.kind() != opal::ErrorKind::kModeled) return result;"), client);
    assertTrue(client.contains("if (error.code() == \"NotFound\")"), client);
    assertTrue(client.contains("bool is_not_found() const"), client);
    assertTrue(
        client.contains(
            "const Quota* as_quota_or_null() const" + " { return std::get_if<2>(&value_); }"),
        client);
    assertTrue(
        client.contains(
            "static constexpr const char* kNames[] ="
                + " {\"(empty)\", \"not_found\", \"quota\"};"),
        client);
    // Error listings are matched from a opal::Error, never hand-assembled:
    // no From<Member> factories.
    assertFalse(client.contains("FromNotFound"), client);
  }

  private static final String ORDERING_MODEL =
      """
      $version: "2.0"
      namespace test.shape
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Ping] }

      @http(method: "POST", uri: "/ping")
      operation Ping { input := { status: Status, size: Size, empty: Empty } }

      union Status { pending: Pending, ready: Ready }
      structure Pending { position: Integer }
      structure Ready { at: Timestamp }
      structure Empty {}
      enum Size { SMALL, LARGE }
      """;

  @Test
  void generatedTypesAreOrderedAndEnumsSwitchDirectly() {
    // Issue #49: generated types offered only operator==, so they couldn't
    // key a std::map, and enums needed .value() before a switch. Structs,
    // unions, and enums now default operator<=> beside operator==, and the
    // enum wrapper converts implicitly to its Value so `switch (size)` works
    // — with explicit Value equality friends so the conversion introduces no
    // overload ambiguity.
    String types =
        PluginTestHarness.generate(ORDERING_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include <compare>"), types);
    assertTrue(
        types.contains("friend auto operator<=>(const Pending&, const Pending&) = default;"),
        types);
    assertTrue(
        types.contains("friend auto operator<=>(const Status&, const Status&) = default;"), types);
    assertTrue(
        types.contains("friend auto operator<=>(const Size&, const Size&) = default;"), types);
    assertTrue(types.contains("operator Value() const { return value_; }"), types);
    assertTrue(
        types.contains("friend bool operator==(const Size& a, Value b) { return a.value_ == b; }"),
        types);
  }

  @Test
  void generatedTypesHashForUnorderedContainers() {
    // Issue #49 follow-up: <=> unblocked ordered containers; unordered ones
    // need std::hash. A type gets std::hash exactly when it gets <=>. The
    // specializations must live at global scope, so they're emitted after the
    // namespace closes, in definition order (nested hashes before outer ones).
    var manifest = PluginTestHarness.generate(ORDERING_MODEL, "test.shape#Svc", "test::shape");
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include \"opal/core/hash.h\""), types);
    // Structs hash member-wise through opal::HashValue (containers and
    // optionals have no std::hash of their own).
    assertTrue(types.contains("struct std::hash<test::shape::Pending> {"), types);
    assertTrue(
        types.contains("seed = opal::HashCombine(seed, opal::HashValue(value.position));"), types);
    // Member-less structs have nothing to mix — and must not name the unused
    // parameter (clang's -Wunused-parameter fires in every including TU).
    assertTrue(
        types.contains(
            "std::size_t operator()(const test::shape::Empty& /*value*/) const noexcept"
                + " { return 0; }"),
        types);
    // Enums hash their private (value, unknown-text) pair; unions hash the
    // engaged index + member — both need the friend declaration.
    assertTrue(types.contains("friend struct std::hash<Size>;"), types);
    assertTrue(types.contains("struct std::hash<test::shape::Size> {"), types);
    assertTrue(types.contains("friend struct std::hash<Status>;"), types);
    assertTrue(types.contains("struct std::hash<test::shape::Status> {"), types);
    // All specializations sit outside the namespace block.
    assertTrue(
        types.indexOf("}  // namespace test::shape") < types.indexOf("std::hash<test::shape::"),
        types);
    // Error listings share the tagged-variant shape, so they hash too.
    String client =
        PluginTestHarness.generate(ERRORS_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/client.h");
    assertTrue(client.contains("struct std::hash<test::shape::PingErrors> {"), client);
  }

  @Test
  void generatedTypesPrintForLoggingAndTests() {
    // Issue #85: one sink primitive per type (AppendDebugTo), with
    // DebugString() and operator<< as thin adapters — designated-initializer
    // style for structs, ToString text for enums, engaged member for unions.
    // Disengaged optionals are omitted; required members print
    // unconditionally.
    var manifest = PluginTestHarness.generate(ORDERING_MODEL, "test.shape#Svc", "test::shape");
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("#include \"opal/core/print.h\""), types);
    assertTrue(types.contains("#include <ostream>"), types);
    assertTrue(types.contains("void AppendDebugTo(std::string& out) const {"), types);
    assertTrue(types.contains("out += \"Pending{\";"), types);
    // Members read through this-> so a member named `out` can't shadow the
    // sink parameter.
    assertTrue(types.contains("if (this->position.has_value()) {"), types);
    assertTrue(types.contains("out += \".position = \";"), types);
    assertTrue(types.contains("opal::DebugAppend(out, *this->position);"), types);
    assertTrue(
        types.contains(
            "std::string DebugString() const {"
                + " std::string out; AppendDebugTo(out); return out; }"),
        types);
    assertTrue(
        types.contains("friend std::ostream& operator<<(std::ostream& os, const Pending& value)"),
        types);
    // Enums print their wire text (ToString covers unknown values too).
    assertTrue(types.contains("out += \"Size(\";"), types);
    assertTrue(types.contains("out += ToString();"), types);
    // Unions print the engaged member by name; the empty state prints none.
    assertTrue(types.contains("out += \"Status(\";"), types);
    assertTrue(types.contains("out += \"pending = \";"), types);
    assertTrue(types.contains("opal::DebugAppend(out, std::get<1>(value_));"), types);
    // A @required member prints unconditionally — no presence guard.
    var required = PluginTestHarness.generate(ERRORS_MODEL, "test.shape#Svc", "test::shape");
    String requiredTypes = required.expectFileString("/include/test/shape/types.h");
    assertTrue(requiredTypes.contains("out += \".id = \";"), requiredTypes);
    assertFalse(requiredTypes.contains("if (id.has_value())"), requiredTypes);
    // Error listings share the tagged-variant printer.
    String client = required.expectFileString("/include/test/shape/client.h");
    assertTrue(client.contains("out += \"PingErrors(\";"), client);
  }

  @Test
  void sensitiveShapesRedactInsteadOfPrinting() {
    // Smithy's @sensitive exists precisely so values don't leak into logs; a
    // printing feature that ignored it would defeat the trait (issue #85,
    // protobuf debug_redact precedent). Members targeting a @sensitive shape
    // print [REDACTED]; a @sensitive aggregate redacts its own body.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping { input := { creds: Creds, vault: Vault } }

        structure Creds { user: String, token: Token }

        @sensitive
        string Token

        @sensitive
        structure Vault { combo: String }
        """;
    String types =
        PluginTestHarness.generate(model, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("out += \"[REDACTED]\";"), types);
    assertFalse(types.contains("DebugAppend(out, *this->token)"), types);
    assertTrue(types.contains("out += \"Vault{[REDACTED]}\";"), types);
    // The non-sensitive sibling member still prints its value.
    assertTrue(types.contains("opal::DebugAppend(out, *this->user);"), types);
  }

  @Test
  void structMemberCollidingWithPrintingNamesFailsWithContext() {
    // Structs previously carried no member functions; AppendDebugTo and
    // DebugString are new reserved names, so a Smithy member folding onto one
    // must fail with an attributed diagnostic, not a C++ redeclaration error.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping { input := { DebugString: String } }
        """;
    PluginTestHarness.assertRejected(
        model, "test.shape#Svc", "test::shape", "cpp-codegen", "DebugString");
  }

  @Test
  void nonOrderableMembersSkipTheDefaultedOrdering() {
    // clang hard-errors deducing a deep <=> around a Boxed recursion cycle,
    // and warns on any defaulted-but-deleted operator — so shapes that can't
    // order (recursion, Document members, transitively) get an equality-only
    // comment instead of a defaulted operator<=> (caught by CI on PR #84).
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping {
            input := { tree: Node, wrapper: Wrapper, plain: Plain }
            errors: [Opaque]
        }

        structure Node { value: Integer, next: Node }
        structure Meta { data: Document }
        structure Wrapper { meta: Meta }
        structure Plain { id: String }

        @error("client")
        @httpError(400)
        structure Opaque { data: Document }
        """;
    var manifest = PluginTestHarness.generate(model, "test.shape#Svc", "test::shape");
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(
        types.contains("friend bool operator==(const Node&, const Node&) = default;"), types);
    assertFalse(types.contains("operator<=>(const Node&"), types);
    assertFalse(types.contains("operator<=>(const Meta&"), types);
    assertFalse(types.contains("operator<=>(const Wrapper&"), types);
    assertTrue(types.contains("// Equality-only: a member type has no ordering"), types);
    // Non-orderability propagates: the input struct contains Node/Wrapper, so
    // it skips too — while the plain sibling keeps its ordering.
    assertFalse(types.contains("operator<=>(const PingInput&"), types);
    assertTrue(
        types.contains("friend auto operator<=>(const Plain&, const Plain&) = default;"), types);
    // Printing is NOT gated like ordering/hashing: value semantics keep the
    // data acyclic, so recursive and Document-bearing shapes print fine.
    assertTrue(types.contains("out += \"Node{\";"), types);
    assertTrue(types.contains("out += \"Wrapper{\";"), types);
    // Hashing follows ordering: non-orderable shapes get no std::hash either,
    // transitively — while the plain sibling keeps its specialization.
    assertFalse(types.contains("std::hash<test::shape::Node>"), types);
    assertFalse(types.contains("std::hash<test::shape::Wrapper>"), types);
    assertFalse(types.contains("std::hash<test::shape::PingInput>"), types);
    assertTrue(types.contains("struct std::hash<test::shape::Plain> {"), types);
    // The client's error listing skips too when a modeled error can't order.
    String client = manifest.expectFileString("/include/test/shape/client.h");
    assertFalse(client.contains("operator<=>(const PingErrors&"), client);
    assertTrue(client.contains("// Equality-only: a member type has no ordering"), client);
    assertFalse(client.contains("std::hash<test::shape::PingErrors>"), client);
  }

  @Test
  void typedErrorListingNameCollisionFailsWithContext() {
    // The listing's synthetic name <Op>Errors can collide with a modeled
    // shape; that must be an attributed cpp-codegen diagnostic, not silent
    // misgeneration. (The plural dodges the real DescribeSink /
    // DescribeSinkError fixture; this pins the guard for the plural too.)
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping {
            input := { @required id: String, extra: PingErrors }
            errors: [NotFound]
        }

        structure PingErrors { note: String }

        @error("client")
        @httpError(404)
        structure NotFound { message: String }
        """;
    PluginTestHarness.assertRejected(
        model, "test.shape#Svc", "test::shape", "cpp-codegen", "PingErrors");
  }

  @Test
  void streamingBlobsStayPlainBufferedBlobs() {
    // The README's "Current limitations": @streaming BLOBS remain unmodeled —
    // a streaming blob payload generates as an ordinary, fully buffered
    // opal::Blob with the plain unary operation around it. Event-stream
    // unions became real in Phase 8 slice 3 (ADR-0016; the flipped pin is
    // eventStreamOperationsGenerateStreamingSignatures below), which is why
    // this pin is now blob-specific.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Upload] }

        @http(method: "POST", uri: "/upload")
        operation Upload {
            input := {
                @httpPayload
                @required
                body: StreamingBlob
            }
            output := { @required etag: String }
        }

        @streaming
        blob StreamingBlob
        """;
    var manifest = PluginTestHarness.generate(model, "test.shape#Svc", "test::shape");
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("opal::Blob body{};"), types);
    String client = manifest.expectFileString("/include/test/shape/client.h");
    assertTrue(
        client.contains("opal::Outcome<UploadOutput> Upload(const UploadInput& input) const;"),
        client);
    assertFalse(client.contains("EventStream"), client);
  }

  @Test
  void streamingBlobOutputsTakeAWriterAndAreGatedOnSuccess() {
    // #213 slice 2. A @streaming blob in the *response* streams to a writer
    // the caller supplies rather than materializing in the output. Requests
    // keep the pin above: writing one needs chunked request framing, which
    // the http1 codec refuses on purpose.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Download] }

        @readonly
        @http(method: "GET", uri: "/download/{id}")
        operation Download {
            input := {
                @required
                @httpLabel
                id: String
            }
            output := {
                @httpHeader("ETag")
                etag: String

                @required
                @httpPayload
                content: StreamingBlob
            }
        }

        @streaming
        blob StreamingBlob
        """;
    var manifest = PluginTestHarness.generate(model, "test.shape#Svc", "test::shape");

    // The writer is defaulted, so an operation that was callable before still
    // is, and omitting it buffers exactly as it used to.
    String client = manifest.expectFileString("/include/test/shape/client.h");
    assertTrue(
        client.contains(
            "opal::Outcome<DownloadOutput> Download(const DownloadInput& input, "
                + "const opal::http::BodyWriter& write = nullptr) const;"),
        client);

    // The accept gate belongs to the generated code, and it is the operation's
    // own success condition spelled once more: a modeled code here, so exactly
    // that code streams. A wider gate would stream a status the client is
    // about to reject, leaving the error path an empty body to parse.
    String source = manifest.expectFileString("/src/client.cc");
    assertTrue(
        source.contains(
            ".accept = [](int status, const opal::http::Headers&) " + "{ return status == 200; },"),
        source);
    assertTrue(source.contains(".write = write,"), source);
    assertTrue(source.contains("auto response = Send(std::move(request), payload_sink);"), source);

    // A null writer has to reach the buffered path, or omitting the argument
    // would turn every such call into an empty-sink streaming send.
    assertTrue(source.contains("if (sink.write == nullptr) {"), source);

    // The member stays on the output struct. It is shared with the server
    // generator, which still returns the payload; removing it would drag the
    // deferred server half into this change. (Smithy requires @required or
    // @default on a streaming member, so it is a plain Blob — left empty when
    // the bytes went to the writer instead.)
    String types = manifest.expectFileString("/include/test/shape/types.h");
    assertTrue(types.contains("opal::Blob content"), types);
  }

  @Test
  void aStreamingPayloadUnderHttpResponseCodeStreamsEverySuccessStatus() {
    // The other success rule (#213 slice 2, cursor review on PR 216). With
    // @httpResponseCode the service picks the status, so success is 2xx or
    // 3xx — a modeled redirect that carries a payload is a success, and a gate
    // keyed on 2xx would silently buffer it into the member while still
    // returning success, which is not what the caller asked for.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Download] }

        @readonly
        @http(method: "GET", uri: "/download/{id}")
        operation Download {
            input := {
                @required
                @httpLabel
                id: String
            }
            output := {
                @required
                @httpResponseCode
                status: Integer

                @required
                @httpPayload
                content: StreamingBlob
            }
        }

        @streaming
        blob StreamingBlob
        """;
    var manifest = PluginTestHarness.generate(model, "test.shape#Svc", "test::shape");

    String source = manifest.expectFileString("/src/client.cc");
    assertTrue(
        source.contains(
            ".accept = [](int status, const opal::http::Headers&) "
                + "{ return status >= 200 && status < 400; },"),
        source);
    // The gate and the check below it are the same predicate; if they ever
    // disagree, one of the two states above is unreachable or wrong.
    assertTrue(
        source.contains("if (response->status < 200 || response->status >= 400) return"), source);
  }

  @Test
  void eventStreamOperationsGenerateStreamingSignatures() {
    // The flip of the old "@streaming is ignored" pin (ADR-0016): an
    // event-stream union on an operation now generates the typed-session
    // signature on both wire ends. The full surface (codecs, routes,
    // diagnostics, BUILD deps) is EventStreamGeneratorTest's.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Chat] }

        @http(method: "POST", uri: "/chat")
        operation Chat {
            input := { @httpPayload events: In }
            output := { @httpPayload events: Out }
        }

        @streaming
        union In { ping: Ping }

        @streaming
        union Out { pong: Pong }

        structure Ping { text: String }
        structure Pong { text: String }
        """;
    var manifest = PluginTestHarness.generate(model, "test.shape#Svc", "test::shape");
    String client = manifest.expectFileString("/include/test/shape/client.h");
    assertTrue(
        client.contains("using ChatClientStream = opal::eventstream::EventStream<In, Out>;"),
        client);
    assertTrue(
        client.contains("opal::Outcome<ChatClientStream> Chat(const ChatInput& input) const;"),
        client);
    String server = manifest.expectFileString("/include/test/shape/server.h");
    assertTrue(
        server.contains("using ChatServerStream = opal::eventstream::EventStream<Out, In>;"),
        server);
    assertTrue(
        server.contains(
            "virtual opal::Outcome<opal::Unit> Chat(const ChatInput& input,"
                + " ChatServerStream& stream,"
                + " const opal::server::RequestContext& context) = 0;"),
        server);
  }

  @Test
  void paginatorsAreRanges() {
    // Issue #49: @paginated was pull-only — Next()/nullopt with no
    // begin()/end(), so `for (page : pages)` didn't compile. Paginators now
    // expose the single-pass opal::PageIterator range: iteration yields
    // Outcome<Page>&, and a failed call ends the range after being seen once.
    String model =
        """
        $version: "2.0"
        namespace test.shape
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [ListThings] }

        @paginated(inputToken: "nextToken", outputToken: "nextToken")
        @readonly
        @http(method: "GET", uri: "/things")
        operation ListThings {
            input := { @httpQuery("nextToken") nextToken: String }
            output := { nextToken: String, things: StringList }
        }

        list StringList { member: String }
        """;
    String client =
        PluginTestHarness.generate(model, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/client.h");
    assertTrue(client.contains("#include \"opal/client/pagination.h\""), client);
    assertTrue(client.contains("using Page = ListThingsOutput;"), client);
    assertTrue(
        client.contains(
            "opal::PageIterator<ListThingsPaginator> begin() { return"
                + " opal::PageIterator<ListThingsPaginator>(this); }"),
        client);
    assertTrue(
        client.contains("opal::PageIterator<ListThingsPaginator> end() { return {}; }"), client);
  }

  @Test
  void httpsWithoutTransportPointsAtFromConfig() {
    // Issue #49 (knob placement): production TLS/pool knobs live on
    // ClientConfig, and BeastHttpClient::FromConfig is the one-stop
    // construction path — so the https fail-fast in Create() must point
    // there, not at the removed FromEndpoint.
    String client =
        PluginTestHarness.generate(ERRORS_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/src/client.cc");
    assertTrue(
        client.contains(
            "https endpoints need a TLS-capable transport"
                + " (set config.http_client, e.g. opal::http::BeastHttpClient::FromConfig)"),
        client);
    assertFalse(client.contains("FromEndpoint"), client);
  }

  @Test
  void noInputRpcv2CborRouteDecodesNoBody() {
    // #67 removed the dead body-decode from no-input server routes (clients
    // never send one; the decode could only 400 conforming empty bodies with
    // stray content). With only a no-input operation, the server must contain
    // no CBOR decode at all — its one route goes straight to the handler.
    String server =
        PluginTestHarness.generate(
                NO_INPUT_MODEL_TEMPLATE.formatted("smithy.protocols#rpcv2Cbor", "rpcv2Cbor"),
                "test.shape#Svc",
                "test::shape")
            .expectFileString("/src/server.cc");
    assertTrue(server.contains("/service/Svc/operation/Ping"), server);
    assertFalse(server.contains("cbor::Decode"), server);
  }

  @Test
  void noInputJsonRpc2DispatchNeverDeserializesParams() {
    // Same #67 fix on the jsonRpc2 side: the no-input Handle<Op> ignores the
    // params member instead of deserializing it into the empty input.
    String server =
        PluginTestHarness.generate(
                NO_INPUT_MODEL_TEMPLATE.formatted("smithy.cpp.protocols#jsonRpc2", "jsonRpc2"),
                "test.shape#Svc",
                "test::shape")
            .expectFileString("/src/server.cc");
    assertTrue(server.contains("(void)params;"), server);
    assertFalse(server.contains("DeserializePingInput"), server);
  }

  @Test
  void serverHandlerDocumentsTheConcurrentDispatchContract() {
    // BeastServerTransport dispatches handlers concurrently from a dedicated
    // executor (issue #46): the generated interface is where consumers learn
    // the contract their implementations must meet. If this doc line drifts,
    // the executor silently races user code that was never warned.
    String server =
        PluginTestHarness.generate(UNION_MODEL, "test.shape#Svc", "test::shape")
            .expectFileString("/include/test/shape/server.h");
    assertTrue(server.contains("Implementations must be thread-safe"), server);
    assertTrue(server.contains("operations concurrently on the one handler instance"), server);
  }
}
