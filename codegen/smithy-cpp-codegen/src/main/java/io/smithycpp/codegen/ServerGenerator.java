package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.DocumentationTrait;

/**
 * Emits server.h / src/server.cc: the pure-virtual {@code <Service>Handler} interface users
 * implement and the {@code <Service>Server} that binds it to the runtime router. Transport agnostic
 * — the resulting {@code opal::http::RequestHandler} plugs into any {@code HttpServerTransport}
 * (Loopback, SocketHttpServer, BeastServerTransport).
 */
final class ServerGenerator {

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;
  private final List<OperationShape> operations;

  ServerGenerator(
      CppContext context,
      ServiceShape service,
      ProtocolGenerator protocol,
      List<OperationShape> operations) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
    this.operations = operations;
  }

  private String serviceName() {
    return CppReservedWords.escape(service.getId().getName());
  }

  /** The event-streaming subset (ADR-0016); empty for unary-only services. */
  private List<OperationShape> streamingOperations() {
    return EventStreamCodeGen.streamingOperations(context.model(), operations);
  }

  /** The unary complement of {@link #streamingOperations()}. */
  private List<OperationShape> unaryOperations() {
    return operations.stream()
        .filter(op -> !EventStreamCodeGen.streaming(context.model(), op))
        .toList();
  }

  void run() {
    rejectRouteConflicts();
    context
        .writerDelegator()
        .useFileWriter(context.settings().serverHeaderFile(), this::writeHeader);
    context.writerDelegator().useFileWriter("src/server.cc", this::writeSource);
  }

  /**
   * Two operations whose method + URI pattern shape coincide (labels are interchangeable for
   * matching purposes) could never be routed apart; fail generation instead of emitting a server
   * that silently drops one of them.
   */
  private void rejectRouteConflicts() {
    java.util.Map<String, OperationShape> seen = new java.util.HashMap<>();
    for (OperationShape operation : operations) {
      var http = operation.getTrait(software.amazon.smithy.model.traits.HttpTrait.class);
      if (http.isEmpty()) {
        continue; // rpcv2Cbor routes are keyed by operation name and cannot collide.
      }
      StringBuilder shape = new StringBuilder(http.get().getMethod());
      for (var segment : http.get().getUri().getSegments()) {
        shape.append('/');
        if (segment.isGreedyLabel()) {
          shape.append("{+}");
        } else if (segment.isLabel()) {
          shape.append("{}");
        } else {
          shape.append(segment.getContent());
        }
      }
      OperationShape existing = seen.putIfAbsent(shape.toString(), operation);
      if (existing != null) {
        throw new software.amazon.smithy.codegen.core.CodegenException(
            "cpp-codegen: ambiguous routes: "
                + existing.getId()
                + " and "
                + operation.getId()
                + " share the route shape '"
                + shape
                + "'");
      }
    }
  }

  private void writeHeader(CppWriter w) {
    w.addInclude("<memory>");
    w.addInclude("\"" + context.settings().includePrefix() + "/types.h\"");
    w.addInclude("\"smithy/core/outcome.h\"");
    w.addInclude("\"smithy/http/transport.h\"");
    w.addInclude("\"smithy/server/router.h\"");
    boolean hasStreaming = !streamingOperations().isEmpty();
    if (hasStreaming) {
      w.addInclude("\"smithy/eventstream/event_stream.h\"");
      w.addInclude("\"smithy/eventstream/async_event_stream.h\"");
      w.addInclude("\"smithy/server/websocket_router.h\"");
    }

    String name = serviceName();
    if (hasStreaming) {
      EventStreamCodeGen.writeStreamAliases(
          w, context, service, streamingOperations(), /* serverSide= */ true);
    }
    w.write("/// Implement one method per operation. Return a modeled error as");
    w.write("/// opal::Error::Modeled(\"<ErrorShapeName>\", message), optionally with the");
    w.write("/// typed error structure attached via set_detail() so it serializes fully.");
    w.write("/// The context carries the raw request and routing captures — see");
    w.write("/// opal::server::RequestContext; leave the parameter unnamed when unused.");
    w.write("/// Implementations must be thread-safe: transports may invoke any mix of");
    w.write("/// operations concurrently on the one handler instance.");
    w.openBlock("class $LHandler {", name);
    w.write("public:").indent();
    w.write("virtual ~$LHandler() = default;", name);
    w.write("");
    for (OperationShape operation : operations) {
      boolean documented = writeModelDocs(w, operation);
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // Streaming operations (ADR-0016): input first, context last, the
        // ADR-0010 shape, with the borrowed session in between.
        if (documented) {
          w.write("///"); // blank separator: model docs above, boilerplate below
        }
        if (EventStreamCodeGen.inputInfo(context.model(), operation).isEmpty()) {
          w.write("/// Streaming operation (ADR-0016): this operation models no");
          w.write("/// client-to-server events, so drive the session with Send (a Receive");
          w.write("/// only ever reports the client's close). Return Unit for a clean");
          w.write("/// close — or an error, which ends the stream with an exception");
          w.write("/// message before the close.");
        } else {
          w.write("/// Streaming operation (ADR-0016): Send/Receive on `stream` until done,");
          w.write("/// then return Unit for a clean close — or an error, which ends the");
          w.write("/// stream with an exception message before the close (unless the");
          w.write("/// stream already terminated: propagating a failed Receive() closes");
          w.write("/// without a message — the peer already observed that failure).");
        }
        w.write("/// `stream` is valid only until this method returns; join any helper");
        w.write("/// thread still using it. Blocks the transport's handler thread for");
        w.write("/// the session's lifetime.");
        w.write(
            "virtual opal::Outcome<opal::Unit> $L(const $L& input, $L& stream, $L context)"
                + " = 0;",
            CppReservedWords.escape(operation.getId().getName()),
            context.cppSymbols().toSymbol(input).getName(),
            EventStreamCodeGen.serverStreamAlias(operation),
            ProtocolSupport.REQUEST_CONTEXT_PARAM);
        continue;
      }
      writeUnaryVirtual(w, operation);
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
    if (hasStreaming) {
      writeAsyncHandlerClass(w, name);
    }
    w.write(
        "/// $L server for $L: routing, deserialization, handler dispatch,",
        protocol.name(),
        service.getId().toString());
    w.write("/// response serialization, and modeled-error mapping. Pass Handler() to any");
    w.write("/// opal::http::HttpServerTransport.");
    w.openBlock("class $LServer {", name);
    w.write("public:").indent();
    w.write("explicit $LServer(std::shared_ptr<$LHandler> handler);", name, name);
    if (hasStreaming) {
      w.write("/// The zero-thread alternative (ADR-0021): the unary table is identical,");
      w.write("/// and every streaming route is registered on the shared-session seam,");
      w.write("/// served by coroutine. One server instance serves one seam — the");
      w.write("/// constructor chosen decides which two-line mount applies (StreamRouter).");
      w.write("explicit $LServer(std::shared_ptr<$LAsyncHandler> handler);", name, name);
    }
    w.write("");
    w.write("opal::http::RequestHandler Handler() const;");
    if (hasStreaming) {
      w.write("");
      w.write("/// The WebSocket router carrying every streaming route (ADR-0016), ready");
      w.write("/// to mount on the transport in two lines — the serve line keyed to the");
      w.write("/// constructor used:");
      w.write("///   options.websocket_gate = server.StreamRouter()->Gate();");
      w.write("///   options.on_websocket = server.StreamRouter()->Serve();  // blocking handler");
      w.write("///   options.on_websocket_session =");
      w.write("///       server.StreamRouter()->ServeSession();  // async handler (ADR-0021)");
      w.write("std::shared_ptr<opal::server::WebSocketRouter> StreamRouter() const;");
    }
    w.write("").dedent();
    w.write("private:").indent();
    w.write("std::shared_ptr<opal::server::Router> router_;");
    if (hasStreaming) {
      w.write("std::shared_ptr<opal::server::WebSocketRouter> stream_router_;");
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
  }

  /** Emits the operation's model docs; returns whether any were written. */
  private boolean writeModelDocs(CppWriter w, OperationShape operation) {
    boolean documented = operation.hasTrait(DocumentationTrait.class);
    operation
        .getTrait(DocumentationTrait.class)
        .ifPresent(
            docs -> {
              for (String line : docs.getValue().split("\n", -1)) {
                w.write("/// $L", line);
              }
            });
    return documented;
  }

  /**
   * One unary operation's blocking virtual. Shared by both handler classes so ADR-0021's "unary
   * operations keep their blocking signatures verbatim" is structural, not aspirational.
   */
  private void writeUnaryVirtual(CppWriter w, OperationShape operation) {
    w.write(
        "virtual opal::Outcome<$L> $L(const $L& input, $L context) = 0;",
        context.cppSymbols().toSymbol(ProtocolSupport.outputShape(context, operation)).getName(),
        CppReservedWords.escape(operation.getId().getName()),
        context.cppSymbols().toSymbol(ProtocolSupport.inputShape(context, operation)).getName(),
        ProtocolSupport.REQUEST_CONTEXT_PARAM);
  }

  /**
   * The coroutine handler surface (ADR-0021): streaming operations return a StreamTask the
   * generated launch wrapper awaits; unary operations keep their blocking signatures verbatim, so
   * one handler object serves the whole service on the shared-session seam.
   */
  private void writeAsyncHandlerClass(CppWriter w, String name) {
    w.write("/// $LHandler's zero-thread sibling (ADR-0021): implement this and construct", name);
    w.write("/// $LServer with it to serve every streaming route on the shared-session", name);
    w.write("/// seam — no parked thread per session. Unary operations keep the blocking");
    w.write("/// shape (request/response on the handler pool either way). Implementations");
    w.write("/// must be thread-safe, like $LHandler.", name);
    w.openBlock("class $LAsyncHandler {", name);
    w.write("public:").indent();
    w.write("virtual ~$LAsyncHandler() = default;", name);
    w.write("");
    for (OperationShape operation : operations) {
      boolean documented = writeModelDocs(w, operation);
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        if (documented) {
          w.write("///"); // blank separator: model docs above, boilerplate below
        }
        if (EventStreamCodeGen.inputInfo(context.model(), operation).isEmpty()) {
          w.write("/// Async streaming operation (ADR-0021): a coroutine serving the whole");
          w.write("/// session. No client-to-server events are modeled, so park in");
          w.write("/// `co_await stream.Receive()` to learn the client closed, and push");
          w.write("/// through stream.Share() (typically a registry) — a Send-only loop on");
          w.write("/// a quiet stream never notices the client left.");
        } else {
          w.write("/// Async streaming operation (ADR-0021): a coroutine serving the whole");
          w.write("/// session — co_await stream.Receive()/Send() until done.");
        }
        w.write("/// co_return opal::Unit{} for a clean close, or an error — modeled as");
        w.write("/// opal::Error::Modeled(\"<ErrorShapeName>\", message) + set_detail(),");
        w.write("/// like a blocking handler — which ends the stream with one best-effort");
        w.write("/// exception message before the close. `input` is the coroutine's own");
        w.write("/// copy: the upgrade request (and its RequestContext) is gone by the");
        w.write("/// first resumption — model what the session needs as input members and");
        w.write("/// enforce identity at the gate. `stream` stays valid until the returned");
        w.write("/// task completes. Code before the first co_await runs on the launching");
        w.write("/// handler thread (brief blocking is fine there); every later resumption");
        w.write("/// is a transport completion context — never block those.");
        w.write(
            "virtual opal::eventstream::StreamTask $L($L input, $L& stream) = 0;",
            CppReservedWords.escape(operation.getId().getName()),
            context.cppSymbols().toSymbol(input).getName(),
            EventStreamCodeGen.asyncServerStreamAlias(operation));
        continue;
      }
      writeUnaryVirtual(w, operation);
    }
    w.dedent();
    w.closeBlock("};");
    w.write("");
  }

  private void writeSource(CppWriter w) {
    for (String include : ProtocolSupport.sharedServerIncludes(context)) {
      w.addInclude(include);
    }
    for (String include : protocol.serverIncludes()) {
      w.addInclude(include);
    }
    String name = serviceName();
    boolean hasStreaming = !streamingOperations().isEmpty();

    ProtocolSupport.openHelpersNamespace(w);
    protocol.writeServerHelpers(w, context, service, operations);
    if (hasStreaming) {
      EventStreamCodeGen.writeServerStreamHelpers(
          w, context, service, protocol, streamingOperations());
    }
    ProtocolSupport.closeHelpersNamespace(w);

    w.openBlock("$LServer::$LServer(std::shared_ptr<$LHandler> handler)", name, name, name);
    if (hasStreaming) {
      w.write(": router_(std::make_shared<opal::server::Router>()),");
      w.write("  stream_router_(std::make_shared<opal::server::WebSocketRouter>()) {");
    } else {
      w.write(": router_(std::make_shared<opal::server::Router>()) {");
    }
    w.dedent();
    w.indent();
    w.write("// The route table is derived from the model's @http traits; conflicts are");
    w.write("// a modeling error surfaced by Router::Add (checked at generation time in a");
    w.write("// later phase), so registration results are intentionally discarded.");
    protocol.writeServerRoutes(w, context, service, unaryOperations());
    if (hasStreaming) {
      w.write("// Streaming routes (ADR-0016) live on the WebSocket router; the upgrade");
      w.write("// path bypasses the HTTP chain (ADR-0015), so they never collide with");
      w.write("// the unary table above.");
      protocol.writeStreamServerRoutes(w, context, service, streamingOperations());
    }
    w.dedent();
    w.write("}");
    w.write("");

    if (hasStreaming) {
      w.openBlock("$LServer::$LServer(std::shared_ptr<$LAsyncHandler> handler)", name, name, name);
      w.write(": router_(std::make_shared<opal::server::Router>()),");
      w.write("  stream_router_(std::make_shared<opal::server::WebSocketRouter>()) {");
      w.dedent();
      w.indent();
      w.write("// The same unary table; every streaming route rides the shared-session");
      w.write("// seam (ADR-0021) — an AddSession launch point per operation, so serving");
      w.write("// a stream parks no handler thread.");
      protocol.writeServerRoutes(w, context, service, unaryOperations());
      protocol.writeStreamSessionRoutes(w, context, service, streamingOperations());
      w.dedent();
      w.write("}");
      w.write("");
    }

    w.openBlock("opal::http::RequestHandler $LServer::Handler() const {", name);
    w.write("auto router = router_;");
    w.write(
        "return [router](const opal::http::HttpRequest& request) "
            + "{ return router->Route(request); };");
    w.closeBlock("}");
    w.write("");

    if (hasStreaming) {
      w.openBlock(
          "std::shared_ptr<opal::server::WebSocketRouter> $LServer::StreamRouter() const {", name);
      w.write("return stream_router_;");
      w.closeBlock("}");
      w.write("");
    }
  }
}
