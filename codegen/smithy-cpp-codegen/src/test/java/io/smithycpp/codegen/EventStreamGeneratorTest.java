package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;

/**
 * The generated event-stream surface (ADR-0016): streaming detection, the client and server
 * signatures and codec bodies, the WebSocket routes, the scope diagnostics, and the BUILD dep
 * gating. String-contains pins over {@link PluginTestHarness} runs, like the sibling suites.
 */
class EventStreamGeneratorTest {

  /**
   * A bidirectional chat-like operation with every allowed initial-request binding, plus a
   * server-push one (no input stream — the NoEvents direction). POST on the bidi operation: the
   * modeled method is free (upgrades are GETs on the wire), and GET plus @httpPayload trips
   * Smithy's own HttpMethodSemantics danger at assembly.
   */
  private static final String REST_MODEL =
      """
      $version: "2.0"
      namespace test.stream
      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Converse, Watch] }

      @http(method: "POST", uri: "/rooms/{room}/converse")
      operation Converse {
          input := {
              @required
              @httpLabel
              room: String

              @httpQuery("since")
              since: Integer

              @httpHeader("x-client")
              client: String

              @httpPayload
              events: ClientEvents
          }
          output := {
              @httpPayload
              events: ServerEvents
          }
          errors: [RoomGone]
      }

      @readonly
      @http(method: "GET", uri: "/watch")
      operation Watch {
          output := {
              @httpPayload
              events: ServerEvents
          }
      }

      @streaming
      union ClientEvents {
          message: ChatMessage
      }

      @streaming
      union ServerEvents {
          message: ChatMessage
          joined: MemberJoined
      }

      structure ChatMessage { @required text: String }
      structure MemberJoined { @required member: String }

      @error("client")
      @httpError(410)
      structure RoomGone { message: String }
      """;

  /** The rpcv2Cbor face: stream-only inputs, since the fixed URI carries no initial members. */
  private static final String CBOR_MODEL =
      """
      $version: "2.0"
      namespace test.stream
      use smithy.protocols#rpcv2Cbor

      @rpcv2Cbor
      service Svc { version: "1", operations: [Chat] }

      operation Chat {
          input := { events: ClientEvents }
          output := { events: ServerEvents }
          errors: [RoomGone]
      }

      @streaming
      union ClientEvents { message: ChatMessage }

      @streaming
      union ServerEvents { message: ChatMessage }

      structure ChatMessage { @required text: String }

      @error("client")
      structure RoomGone { message: String }
      """;

  private static MockManifest rest() {
    return PluginTestHarness.generate(REST_MODEL, "test.stream#Svc", "test::stream");
  }

  private static MockManifest cbor() {
    return PluginTestHarness.generate(CBOR_MODEL, "test.stream#Svc", "test::stream");
  }

  @Test
  void clientSignaturesCarryTheTypedSessionPerDirection() {
    String client = rest().expectFileString("/include/test/stream/client.h");
    assertTrue(client.contains("#include \"opal/eventstream/event_stream.h\""), client);
    // One named alias per streaming operation; the signatures use it, so
    // consumers never respell the two-parameter template.
    assertTrue(
        client.contains(
            "using ConverseClientStream ="
                + " opal::eventstream::EventStream<ClientEvents, ServerEvents>;"),
        client);
    assertTrue(
        client.contains(
            "opal::Outcome<ConverseClientStream> Converse(const ConverseInput& input) const;"),
        client);
    // Detection is directional: no input stream parameterizes Tx with the
    // runtime's NoEvents (and the empty input still defaults).
    assertTrue(
        client.contains(
            "using WatchClientStream ="
                + " opal::eventstream::EventStream<opal::eventstream::NoEvents,"
                + " ServerEvents>;"),
        client);
    assertTrue(
        client.contains(
            "opal::Outcome<WatchClientStream> Watch(const WatchInput& input = {}) const;"),
        client);
    // The NoEvents direction's doc-comment tells the truth: Send does not
    // compile there, only Receive is meaningful.
    assertTrue(client.contains("models no client-to-server events"), client);
  }

  @Test
  void clientBodyDialsTheUpgradeAndSuppliesTheCodecs() {
    String client = rest().expectFileString("/src/client.cc");
    // The upgrade target resolves labels and query exactly like a unary
    // request; header bindings ride the upgrade GET.
    assertTrue(client.contains("target += opal::http::EncodePathSegment(input.room);"), client);
    assertTrue(client.contains("query.Add(\"since\","), client);
    assertTrue(client.contains("request.headers.Set(\"x-client\", (*input.client));"), client);
    // config.websocket_dialer wins; the Beast dialer is the fallback.
    assertTrue(
        client.contains("if (config.websocket_dialer) return config.websocket_dialer(request);"),
        client);
    assertTrue(
        client.contains("return opal::http::BeastWebSocketClient::Dialer()(request);"), client);
    // Encode: member-name dispatch -> serde -> protocol bytes -> envelope.
    assertTrue(
        client.contains(
            "return opal::eventstream::MakeEventMessage(\"message\", \"application/json\","
                + " opal::Blob::FromString(opal::json::Encode(SerializeChatMessage("
                + "event.as_message()))));"),
        client);
    // Decode: envelope parse, exception dispatch through the Make<Error>Error
    // machinery (generic fallback), then member-name dispatch into the union.
    assertTrue(client.contains("opal::eventstream::ParseEnvelope(message);"), client);
    assertTrue(
        client.contains(
            "if (parsed.code == \"RoomGone\") return helpers::MakeRoomGoneError(response,"
                + " std::move(parsed));"),
        client);
    assertTrue(client.contains("return helpers::GenericError(std::move(parsed));"), client);
    assertTrue(
        client.contains("return types::ServerEvents::FromJoined(*std::move(event));"), client);
    assertTrue(client.contains("\"Converse: unknown event type: \" + envelope->type"), client);
    // Streaming operations never parse an HTTP error response, so the unary
    // Parse<Op>Error dispatcher must not be emitted for them (dead code).
    assertFalse(client.contains("ParseConverseError"), client);
    // The NoEvents transmit direction gets no encoder stub: Send is a
    // compile error there (event_stream.h), so the slot rides empty.
    assertFalse(client.contains("EncodeWatchEvent"), client);
    assertTrue(
        client.contains(
            "return WatchClientStream(*std::move(socket), {}, helpers::DecodeWatchEvent);"),
        client);
  }

  @Test
  void serverGrowsStreamingHandlersAndAStreamRouter() {
    MockManifest manifest = rest();
    String header = manifest.expectFileString("/include/test/stream/server.h");
    assertTrue(
        header.contains(
            "using ConverseServerStream ="
                + " opal::eventstream::EventStream<ServerEvents, ClientEvents>;"),
        header);
    assertTrue(
        header.contains(
            "virtual opal::Outcome<opal::Unit> Converse(const ConverseInput& input,"
                + " ConverseServerStream& stream,"
                + " const opal::server::RequestContext& context) = 0;"),
        header);
    assertTrue(
        header.contains("std::shared_ptr<opal::server::WebSocketRouter> StreamRouter() const;"),
        header);
    assertTrue(
        header.contains("std::shared_ptr<opal::server::WebSocketRouter> stream_router_;"), header);

    String server = manifest.expectFileString("/src/server.cc");
    // Streaming routes register as GET (upgrades are GETs on the wire,
    // whatever the operation models) on the WebSocket router.
    assertTrue(
        server.contains("(void)stream_router_->Add(\"GET\", \"/rooms/{room}/converse\","), server);
    assertTrue(
        server.contains("auto input = helpers::ParseConverseInput(request, context,"), server);
    assertTrue(server.contains("handler->Converse(*input, stream, context);"), server);
    // A handler failure sends the exception message, then the close.
    assertTrue(
        server.contains(
            "(void)socket.Send(helpers::BuildConverseExceptionMessage(outcome.error()));"),
        server);
    assertTrue(server.contains("stream.Close();"), server);
    assertTrue(server.contains("return opal::eventstream::MakeExceptionMessage(type,"), server);
    // Streaming operations answer over the session, never an HTTP response.
    assertFalse(server.contains("BuildConverseResponse"), server);
    assertFalse(server.contains("BuildWatchResponse"), server);
    // Nothing in Svc is constrained, so the stream routes carry no
    // validation refusal — the unary writeRoute guards, mirrored (dead
    // emission stays dead; see streamRoutesGuardValidationLikeUnaryRoutes
    // for the constrained flip side).
    assertFalse(server.contains("if (!validation_failures.empty())"), server);
    // The server's Watch receive direction is NoEvents: the decode stub
    // stays (a message received there is a real, reachable protocol
    // violation), while its transmit direction still encodes for real.
    assertTrue(server.contains("Watch: no events are modeled in this direction"), server);
    assertTrue(
        server.contains(
            "WatchServerStream stream(socket, helpers::EncodeWatchEvent, helpers::DecodeWatchEvent);"),
        server);
  }

  @Test
  void serverGrowsTheAsyncHandlerAndSessionRoutes() {
    // ADR-0021: the coroutine sibling — async aliases, the StreamTask
    // handler, the second constructor wiring AddSession launch points, and
    // the Detached wrapper that frames a failed outcome via SendAsync.
    MockManifest manifest = rest();
    String header = manifest.expectFileString("/include/test/stream/server.h");
    assertTrue(
        header.contains(
            "using ConverseAsyncServerStream ="
                + " opal::eventstream::AsyncEventStream<ServerEvents, ClientEvents>;"),
        header);
    assertTrue(
        header.contains(
            "virtual opal::eventstream::StreamTask Converse(ConverseInput input,"
                + " ConverseAsyncServerStream& stream) = 0;"),
        header);
    // Unary operations keep the blocking shape on the async handler —
    // pinned byte-for-byte against the blocking class's signature.
    assertTrue(header.contains("class SvcAsyncHandler {"), header);
    String neighbor =
        restWithUnaryNeighborAndTests().expectFileString("/include/test/stream/server.h");
    int first = neighbor.indexOf("Ping(const PingInput& input");
    assertTrue(first >= 0, neighbor);
    int second = neighbor.indexOf("Ping(const PingInput& input", first + 1);
    assertTrue(second > first, neighbor);
    String blockingUnary =
        neighbor.substring(
            neighbor.lastIndexOf("virtual", first), neighbor.indexOf(";", first) + 1);
    String asyncUnary =
        neighbor.substring(
            neighbor.lastIndexOf("virtual", second), neighbor.indexOf(";", second) + 1);
    assertTrue(blockingUnary.equals(asyncUnary), blockingUnary + " vs " + asyncUnary);
    assertTrue(
        header.contains("explicit SvcServer(std::shared_ptr<SvcAsyncHandler> handler);"), header);

    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(
        server.contains("(void)stream_router_->AddSession(\"GET\", \"/rooms/{room}/converse\","),
        server);
    // The launch point parses like the blocking route, refuses on the owned
    // socket, and hands off to the wrapper without blocking.
    assertTrue(
        server.contains(
            "(void)socket->Send(helpers::BuildConverseExceptionMessage(input.error()));"),
        server);
    assertTrue(
        server.contains(
            "helpers::ServeConverseAsync(handler, *std::move(input), std::move(socket));"),
        server);
    assertTrue(
        server.contains(
            "opal::eventstream::Detached ServeConverseAsync(std::shared_ptr<types::SvcAsyncHandler>"
                + " handler, types::ConverseInput input,"
                + " std::shared_ptr<opal::http::WebSocket> socket) {"),
        server);
    assertTrue(
        server.contains("auto outcome = co_await handler->Converse(std::move(input), stream);"),
        server);
    // A failed outcome's exception frame is AWAITED — the wrapper frame
    // (and the stream it owns) must outlive the write, since destroying
    // the stream closes the session and a close over a busy wire can
    // cancel the in-flight send — and the close follows it.
    assertTrue(
        server.contains(
            "(void)co_await opal::eventstream::SendMessage(socket,"
                + " helpers::BuildConverseExceptionMessage(outcome.error()));"),
        server);
    assertFalse(server.contains("socket->SendAsync(BuildConverseExceptionMessage"), server);
  }

  @Test
  void streamRoutesGuardValidationLikeUnaryRoutes() {
    // The flip side of the absence pin above: with a constrained initial
    // member, the constrained operation's route validates and refuses over
    // the session (one SerializationException-shaped exception message,
    // then the close); the unconstrained neighbor gets the parse-level
    // check only, never a validator call — exactly writeRoute's guards.
    String model = REST_MODEL.replace("@httpLabel\n", "@httpLabel\n        @length(max: 8)\n");
    String server =
        PluginTestHarness.generate(model, "test.stream#Svc", "test::stream")
            .expectFileString("/src/server.cc");
    assertTrue(
        server.contains("helpers::ValidateConverseInput(*input, \"\", &validation_failures);"),
        server);
    assertTrue(
        server.contains(
            "(void)socket.Send(helpers::BuildConverseExceptionMessage("
                + "opal::Error::Validation(validation_failures.front().message)));"),
        server);
    // The session (async) route refuses identically on its owned socket.
    assertTrue(
        server.contains(
            "(void)socket->Send(helpers::BuildConverseExceptionMessage("
                + "opal::Error::Validation(validation_failures.front().message)));"),
        server);
    assertFalse(server.contains("ValidateWatchInput"), server);
  }

  @Test
  void rpcv2CborStreamsOnTheFixedUpgradeUriWithCborPayloads() {
    MockManifest manifest = cbor();
    String client = manifest.expectFileString("/src/client.cc");
    assertTrue(
        client.contains("request.target = path_prefix_ + \"/service/Svc/operation/Chat\";"),
        client);
    assertTrue(
        client.contains(
            "return opal::eventstream::MakeEventMessage(\"message\", \"application/cbor\","
                + " opal::cbor::Encode(SerializeChatMessage(event.as_message())));"),
        client);
    assertTrue(client.contains("opal::cbor::Decode(envelope->payload)"), client);
    String server = manifest.expectFileString("/src/server.cc");
    assertTrue(
        server.contains("(void)stream_router_->Add(\"GET\", \"/service/Svc/operation/Chat\","),
        server);
    assertTrue(server.contains("handler->Chat(input, stream, context);"), server);
    // The async constructor registers the same fixed URI on the session seam.
    assertTrue(
        server.contains(
            "(void)stream_router_->AddSession(\"GET\", \"/service/Svc/operation/Chat\","),
        server);
    assertTrue(
        server.contains("ServeChatAsync(handler, std::move(input), std::move(socket));"), server);
  }

  /**
   * The jsonRpc2 face (ADR-0023): a shared endpoint, initial-request members riding the opening
   * envelope's params — the first protocol carrying body-bound initial members.
   */
  private static final String JSONRPC_MODEL =
      """
      $version: "2.0"
      namespace test.stream
      use smithy.cpp.protocols#jsonRpc2

      @jsonRpc2
      service Svc { version: "1", operations: [Chat, Watch] }

      operation Chat {
          input := {
              @required
              room: String

              events: ClientEvents
          }
          output := { events: ServerEvents }
          errors: [RoomGone]
      }

      @readonly
      operation Watch {
          output := { events: ServerEvents }
      }

      @streaming
      union ClientEvents { message: ChatMessage }

      @streaming
      union ServerEvents { message: ChatMessage }

      structure ChatMessage { @required text: String }

      @error("client")
      @httpError(410)
      structure RoomGone { message: String }
      """;

  private static MockManifest jsonRpc() {
    return PluginTestHarness.generate(JSONRPC_MODEL, "test.stream#Svc", "test::stream");
  }

  @Test
  void jsonRpc2ClientOpensTheStreamWithTheRequestEnvelope() {
    MockManifest manifest = jsonRpc();
    String client = manifest.expectFileString("/src/client.cc");
    // The shared endpoint on the raw-text wire: same target as the unary
    // POST, the operation rides the opening envelope (ADR-0023).
    assertTrue(client.contains("request.target = path_prefix_ + \"/\";"), client);
    assertTrue(client.contains("request.raw_text_frames = true;"), client);
    // The opening envelope carries the initial-request members as params,
    // with the stream union erased — it is the session, not a member.
    assertTrue(
        client.contains("opal::DocumentMap params = SerializeChatInput(input).as_map();"), client);
    assertTrue(client.contains("params.erase(\"events\");"), client);
    assertTrue(client.contains("envelope.emplace(\"method\", opal::Document(\"Chat\"));"), client);
    assertTrue(client.contains("envelope.emplace(\"id\", opal::Document(1));"), client);
    // The socket is wrapped in the JSON-RPC translation before the typed
    // stream sees it; the codecs are the shared JSON pair.
    assertTrue(client.contains("std::make_shared<opal::eventstream::JsonRpcStreamSocket>"), client);
    assertTrue(
        client.contains(
            "return opal::eventstream::MakeEventMessage(\"message\", \"application/json\","
                + " opal::Blob::FromString(opal::json::Encode(SerializeChatMessage("
                + "event.as_message()))));"),
        client);
    // A no-input operation still opens with (empty) params.
    assertTrue(client.contains("envelope.emplace(\"method\", opal::Document(\"Watch\"));"), client);
  }

  @Test
  void jsonRpc2ServerDispatchesTheOpeningEnvelopeOnBothSeams() {
    MockManifest manifest = jsonRpc();
    String server = manifest.expectFileString("/src/server.cc");
    // One "/" route per seam for the whole service — the unary POST "/"
    // move, transposed to streams (ADR-0023).
    assertTrue(server.contains("(void)stream_router_->Add(\"GET\", \"/\","), server);
    assertTrue(server.contains("(void)stream_router_->AddSession(\"GET\", \"/\","), server);
    assertTrue(server.contains("helpers::ServeJsonRpcStream(*handler, context, socket);"), server);
    assertTrue(
        server.contains("helpers::ServeJsonRpcSession(handler, std::move(socket));"), server);
    // The session driver reads the opening envelope inside the Detached
    // frame — never parking a handler thread — and the blocking driver
    // blocks in Receive, each dispatching on the envelope's method.
    assertTrue(
        server.contains("auto first = co_await opal::eventstream::ReceiveMessage(socket);"),
        server);
    assertTrue(
        server.contains("const JsonRpcOpening opening = helpers::ParseJsonRpcOpening(**first);"),
        server);
    assertTrue(server.contains("if (opening.method == \"Chat\") {"), server);
    // Reserved-code refusals reuse the unary emitter, as raw text.
    assertTrue(
        server.contains(
            "helpers::JsonRpcStreamText(helpers::JsonRpcError(-32601, \"UnknownOperationException\","
                + " \"unknown method: \" + opening.method, {}, opening.id))"),
        server);
    assertTrue(
        server.contains(
            "helpers::JsonRpcStreamText(helpers::JsonRpcError(-32602, \"SerializationException\","
                + " parsed.error().message(), {}, opening.id))"),
        server);
    // Both seams serve through the wrapped socket and end with the terminal
    // response envelope — result on a clean completion, the unary error
    // identity otherwise — never an exception message.
    assertTrue(
        server.contains(
            "opal::eventstream::JsonRpcStreamSocket wrapped(socket, opening.id,"
                + " opal::eventstream::JsonRpcStreamSocket::Role::kServer);"),
        server);
    assertTrue(server.contains("BuildJsonRpcTerminalResult(opening.id)"), server);
    assertTrue(
        server.contains(
            "helpers::JsonRpcStreamText(helpers::ErrorToResponse(outcome.error(), opening.id))"),
        server);
    assertTrue(
        server.contains(
            "opal::eventstream::Detached ServeChatAsync(std::shared_ptr<types::SvcAsyncHandler>"
                + " handler, types::ChatInput input, std::shared_ptr<opal::http::WebSocket> socket,"
                + " opal::Document id) {"),
        server);
    assertFalse(server.contains("BuildChatExceptionMessage"), server);
    assertFalse(server.contains("MakeExceptionMessage"), server);
    // The parsed opening input drops the stream union before the handler
    // sees it — the union is the session.
    assertTrue(server.contains("input.events.reset();"), server);
  }

  @Test
  void jsonRpc2RejectsARequiredStreamMember() {
    // The opening params deserialize through the input's ordinary serde,
    // and params never carry the union — @required would refuse every
    // opening (ADR-0023).
    String model = JSONRPC_MODEL.replace("events: ClientEvents", "@required events: ClientEvents");
    PluginTestHarness.assertRejected(
        model,
        "test.stream#Svc",
        "test::stream",
        "cpp-codegen",
        "drop @required",
        "test.stream#Chat");
  }

  @Test
  void eventHeaderIsRejected() {
    String model =
        CBOR_MODEL.replace(
            "structure ChatMessage { @required text: String }",
            "structure ChatMessage { @eventHeader id: String, @required text: String }");
    PluginTestHarness.assertRejected(
        model, "test.stream#Svc", "test::stream", "cpp-codegen", "@eventHeader", "ChatMessage$id");
  }

  @Test
  void eventPayloadIsRejected() {
    String model =
        CBOR_MODEL.replace(
            "structure ChatMessage { @required text: String }",
            "structure ChatMessage { @eventPayload text: String }");
    PluginTestHarness.assertRejected(
        model,
        "test.stream#Svc",
        "test::stream",
        "cpp-codegen",
        "@eventPayload",
        "ChatMessage$text");
  }

  @Test
  void initialRequestMembersAreRejectedWhereTheUpgradeCannotCarryThem() {
    // The one assemblable shape of a body-bound initial-request member:
    // rpcv2Cbor, whose fixed upgrade URI binds nothing. Under simpleRestJson
    // the equivalent model cannot even assemble — Smithy requires
    // @httpPayload on the stream member and then rejects unbound siblings —
    // so the generator's REST-side check is a defensive invariant, not a
    // testable surface.
    String model =
        CBOR_MODEL.replace(
            "input := { events: ClientEvents }", "input := { room: String, events: ClientEvents }");
    PluginTestHarness.assertRejected(
        model,
        "test.stream#Svc",
        "test::stream",
        "cpp-codegen",
        "initial-request member 'room'",
        "test.stream#Chat");
  }

  @Test
  void initialResponseMembersAreRejectedAsADocumentedDeferral() {
    String model =
        CBOR_MODEL.replace(
            "output := { events: ServerEvents }",
            "output := { greeting: String, events: ServerEvents }");
    PluginTestHarness.assertRejected(
        model,
        "test.stream#Svc",
        "test::stream",
        "cpp-codegen",
        "initial-response member 'greeting'",
        "test.stream#Chat");
  }

  @Test
  void buildDepsGrowOnlyForStreamingServices() {
    String build =
        PluginTestHarness.generate(
                REST_MODEL,
                "test.stream#Svc",
                "test::stream",
                b -> b.withMember("runtimeTarget", "//runtime:core"))
            .expectFileString("/BUILD.bazel");
    // Client: the typed sessions plus the default Beast dialer. Server: the
    // WebSocketRouter lives in :server — no Beast.
    assertTrue(build.contains("\"//runtime:eventstream\""), build);
    int clientTarget = build.indexOf("name = \"client\"");
    int serverTarget = build.indexOf("name = \"server\"");
    String clientDeps = build.substring(clientTarget, serverTarget);
    String serverDeps = build.substring(serverTarget);
    assertTrue(clientDeps.contains("\"//runtime:http_beast\""), build);
    assertTrue(clientDeps.contains("\"//runtime:eventstream\""), build);
    assertTrue(serverDeps.contains("\"//runtime:eventstream\""), build);
    assertFalse(serverDeps.contains("\"//runtime:http_beast\""), build);

    // Unary services are untouched: dep-light consumers pay nothing.
    String unary =
        """
        $version: "2.0"
        namespace test.stream
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping { input := { name: String } }
        """;
    String unaryBuild =
        PluginTestHarness.generate(
                unary,
                "test.stream#Svc",
                "test::stream",
                b -> b.withMember("runtimeTarget", "//runtime:core"))
            .expectFileString("/BUILD.bazel");
    assertFalse(unaryBuild.contains(":eventstream"), unaryBuild);
    assertFalse(unaryBuild.contains(":http_beast"), unaryBuild);
  }

  /** REST_MODEL plus a unary neighbor, generated with the test suites on. */
  private static MockManifest restWithUnaryNeighborAndTests() {
    String model =
        REST_MODEL.replace(
                "service Svc { version: \"1\", operations: [Converse, Watch] }",
                "service Svc { version: \"1\", operations: [Converse, Watch, Ping] }")
            + """

            @http(method: "POST", uri: "/ping")
            operation Ping { input := { name: String } }
            """;
    return PluginTestHarness.generate(
        model,
        "test.stream#Svc",
        "test::stream",
        b ->
            b.withMember("runtimeTarget", "//runtime:core")
                .withMember("testsPackage", "//generated")
                .withMember("integrationTests", true));
  }

  @Test
  void smokeTestsSkipStreamingOperations() {
    String smoke = restWithUnaryNeighborAndTests().expectFileString("/tests/smoke_test.cc");
    // The unary neighbor round-trips; the streaming operations get no
    // unary-shaped test (no minimal output, no round-trip TEST).
    assertTrue(smoke.contains("TEST(SvcSmokeTest, PingRoundTrips)"), smoke);
    assertFalse(smoke.contains("TEST(SvcSmokeTest, ConverseRoundTrips)"), smoke);
    assertFalse(smoke.contains("TEST(SvcSmokeTest, WatchRoundTrips)"), smoke);
    assertFalse(smoke.contains("MinimalConverseOutput"), smoke);
    // The handler subclass still implements the streaming interface, via the
    // close-immediately stub (spelled with the header's session alias).
    assertTrue(smoke.contains("ConverseServerStream& stream"), smoke);
    assertTrue(smoke.contains("stream.Close();"), smoke);
  }

  @Test
  void integrationTestsSkipStreamingOperations() {
    String tests = restWithUnaryNeighborAndTests().expectFileString("/tests/integration_test.cc");
    assertTrue(tests.contains("PingRandomRoundTrips"), tests);
    assertFalse(tests.contains("ConverseRandomRoundTrips"), tests);
    assertFalse(tests.contains("WatchRandomRoundTrips"), tests);
    // No scripted state for streaming operations, and no error test for the
    // streaming-only RoomGone; the scripted handler keeps the interface
    // implemented through the stub.
    assertFalse(tests.contains("lastConverse"), tests);
    assertFalse(tests.contains("RoomGoneMapsAcrossTheWire"), tests);
    assertTrue(tests.contains("ConverseServerStream& stream"), tests);
  }

  @Test
  void unaryServicesEmitNoStreamingSurface() {
    String model =
        """
        $version: "2.0"
        namespace test.stream
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Ping] }

        @http(method: "POST", uri: "/ping")
        operation Ping { input := { name: String } }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.stream#Svc", "test::stream");
    String client = manifest.expectFileString("/include/test/stream/client.h");
    assertFalse(client.contains("event_stream.h"), client);
    String clientSource = manifest.expectFileString("/src/client.cc");
    assertFalse(clientSource.contains("DialStream"), clientSource);
    String server = manifest.expectFileString("/include/test/stream/server.h");
    assertFalse(server.contains("StreamRouter"), server);
    assertFalse(server.contains("websocket_router.h"), server);
  }
}
