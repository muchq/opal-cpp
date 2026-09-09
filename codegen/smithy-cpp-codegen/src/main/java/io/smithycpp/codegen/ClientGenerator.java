package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.DocumentationTrait;

/** Emits client.h / src/client.cc: the <Service>Client class over the protocol generator. */
final class ClientGenerator {

  private final CppContext context;
  private final ServiceShape service;
  private final ProtocolGenerator protocol;

  ClientGenerator(CppContext context, ServiceShape service, ProtocolGenerator protocol) {
    this.context = context;
    this.service = service;
    this.protocol = protocol;
  }

  List<OperationShape> operations() {
    return ProtocolSupport.containedOperations(context.model(), service);
  }

  /** The event-streaming subset (ADR-0016); empty for unary-only services. */
  private List<OperationShape> streamingOperations() {
    return EventStreamCodeGen.streamingOperations(context.model(), operations());
  }

  private String clientName() {
    return CppReservedWords.escape(service.getId().getName()) + "Client";
  }

  void run() {
    context
        .writerDelegator()
        .useFileWriter(context.settings().clientHeaderFile(), this::writeHeader);
    context.writerDelegator().useFileWriter("src/client.cc", this::writeSource);
  }

  private void writeHeader(CppWriter w) {
    w.addInclude("<memory>");
    w.addInclude("<string>");
    w.addInclude("\"" + context.settings().includePrefix() + "/types.h\"");
    w.addInclude("\"smithy/client/config.h\"");
    w.addInclude("\"smithy/core/outcome.h\"");
    w.addInclude("\"smithy/http/transport.h\"");

    String name = clientName();
    if (!streamingOperations().isEmpty()) {
      w.addInclude("\"smithy/eventstream/event_stream.h\"");
    }
    List<OperationShape> paginated =
        operations().stream().filter(op -> pagination(op).isPresent()).toList();
    if (!paginated.isEmpty()) {
      w.addInclude("<optional>");
      w.addInclude("<utility>");
      w.addInclude("\"smithy/client/pagination.h\"");
      for (OperationShape operation : paginated) {
        w.write("class $L;", paginatorName(operation));
      }
      w.write("");
    }
    if (!streamingOperations().isEmpty()) {
      EventStreamCodeGen.writeStreamAliases(
          w, context, service, streamingOperations(), /* serverSide= */ false);
    }
    w.write("/// $L client for $L.", protocol.name(), service.getId().toString());
    w.write("/// Modeled service errors surface as opal::Error with kind kModeled,");
    w.write("/// code() set to the error shape name, and the deserialized error");
    w.write("/// structure attached. Dispatch on them through the per-operation");
    w.write("/// <Operation>Errors listings below rather than comparing code() text.");
    w.openBlock("class $L {", name);
    w.write("public:").indent();
    w.write("/// Fails when the endpoint cannot be parsed and no transport is injected.");
    w.write("static opal::Outcome<$L> Create(opal::ClientConfig config);", name);
    w.write("");
    for (OperationShape operation : operations()) {
      boolean documented = operation.hasTrait(DocumentationTrait.class);
      operation
          .getTrait(DocumentationTrait.class)
          .ifPresent(
              docs -> {
                for (String line : docs.getValue().split("\n", -1)) {
                  w.write("/// $L", line);
                }
              });
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      String inputType = context.cppSymbols().toSymbol(input).getName();
      String outputType =
          context.cppSymbols().toSymbol(ProtocolSupport.outputShape(context, operation)).getName();
      String defaulted = input.members().isEmpty() ? " = {}" : "";
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // Streaming operations (ADR-0016) return the typed session instead of
        // an output structure; the wire contract lives on the runtime type.
        if (documented) {
          w.write("///"); // blank separator: model docs above, boilerplate below
        }
        w.write("/// Opens the operation's event stream over a WebSocket upgrade");
        if (EventStreamCodeGen.inputInfo(context.model(), operation).isEmpty()) {
          w.write("/// (ADR-0016). This operation models no client-to-server events; only");
          w.write("/// Receive is meaningful (nullopt on the peer's clean close — Send does");
          w.write("/// not compile on a NoEvents direction), and a received exception");
          w.write("/// surfaces through Receive() as a modeled error, the unary shape.");
        } else {
          w.write("/// (ADR-0016). Send carries input events, Receive yields output events");
          w.write("/// (nullopt on the peer's clean close), and a received exception");
          w.write("/// surfaces through Receive() as a modeled error, the unary shape.");
        }
        w.write(
            "opal::Outcome<$L> $L(const $L& input$L) const;",
            EventStreamCodeGen.clientStreamAlias(operation),
            CppReservedWords.escape(operation.getId().getName()),
            inputType,
            defaulted);
        continue;
      }
      w.write(
          "opal::Outcome<$L> $L(const $L& input$L) const;",
          outputType,
          CppReservedWords.escape(operation.getId().getName()),
          inputType,
          defaulted);
      if (pagination(operation).isPresent()) {
        w.write(
            "/// Pages $L until the service stops returning a next token (@paginated).",
            CppReservedWords.escape(operation.getId().getName()));
        w.write(
            "$L Paginate$L($L input$L) const;",
            paginatorName(operation),
            CppReservedWords.escape(operation.getId().getName()),
            inputType,
            defaulted);
      }
    }
    w.write("").dedent();
    w.write("private:").indent();
    w.write(
        "$L(opal::ClientConfig config, std::shared_ptr<opal::http::HttpClient> "
            + "transport, std::string path_prefix);",
        name);
    w.write(
        "opal::Outcome<opal::http::HttpResponse> "
            + "Send(opal::http::HttpRequest request) const;");
    w.write("");
    w.write("opal::ClientConfig config_;");
    w.write("std::shared_ptr<opal::http::HttpClient> transport_;");
    w.write("std::string path_prefix_;").dedent();
    w.closeBlock("};");
    w.write("");

    for (OperationShape operation : paginated) {
      String opName = CppReservedWords.escape(operation.getId().getName());
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      String inputType = context.cppSymbols().toSymbol(input).getName();
      String outputType =
          context.cppSymbols().toSymbol(ProtocolSupport.outputShape(context, operation)).getName();
      w.write("/// Lazily pages $L; owns a copy of the client and the input.", opName);
      w.openBlock("class $L {", paginatorName(operation));
      w.write("public:").indent();
      w.write("/// The next page, std::nullopt once pagination is complete, or the");
      w.write("/// first failed call's error (pagination then stops).");
      w.write("opal::Outcome<std::optional<$L>> Next();", outputType);
      w.write("");
      w.write("using Page = $L;", outputType);
      w.write("/// Single-pass range over pages — contract in smithy/client/pagination.h.");
      String iterator = "opal::PageIterator<" + paginatorName(operation) + ">";
      w.write("$1L begin() { return $1L(this); }", iterator);
      w.write("$L end() { return {}; }", iterator);
      w.write("").dedent();
      w.write("private:").indent();
      w.write("friend class $L;", name);
      w.write(
          "$L($L client, $L input) : client_(std::move(client)), input_(std::move(input)) {}",
          paginatorName(operation),
          name,
          inputType);
      w.write("$L client_;", name);
      w.write("$L input_;", inputType);
      w.write("bool done_ = false;").dedent();
      w.closeBlock("};");
      w.write("");
    }

    for (OperationShape operation : operations()) {
      writeErrorListing(w, operation);
    }
  }

  /**
   * The typed view over an operation's modeled errors (issue #49): a tagged-variant listing so
   * consumers dispatch on FromError(outcome.error()) — exhaustively via visit(), typo-proof via
   * is_x/as_x — instead of comparing Error::code() text and guessing the detail type.
   */
  private void writeErrorListing(CppWriter w, OperationShape operation) {
    List<StructureShape> errors =
        operation.getErrors().stream()
            .map(id -> context.model().expectShape(id, StructureShape.class))
            .toList();
    if (errors.isEmpty()) {
      return;
    }
    String listingName = errorsName(operation);
    requireNoModelCollision(operation, listingName);

    java.util.LinkedHashMap<String, String> folded = new java.util.LinkedHashMap<>();
    for (StructureShape error : errors) {
      folded.put(
          error.getId().getName(),
          "as_" + software.amazon.smithy.utils.CaseUtils.toSnakeCase(error.getId().getName()));
    }
    TypeGenerators.requireDistinctNames("operation", operation.getId(), folded, java.util.Set.of());

    List<TypeGenerators.TaggedMember> members =
        errors.stream()
            .map(
                error ->
                    new TypeGenerators.TaggedMember(
                        software.amazon.smithy.utils.CaseUtils.toSnakeCase(error.getId().getName()),
                        context.cppSymbols().toSymbol(error).getName(),
                        error.hasTrait(software.amazon.smithy.model.traits.SensitiveTrait.class)))
            .toList();

    String opName = CppReservedWords.escape(operation.getId().getName());
    w.write("/// The modeled errors of $L, matched from a opal::Error so dispatch is", opName);
    w.write("/// typed and exhaustive instead of string-compared. FromError() is empty()");
    w.write("/// when the error is none of this operation's modeled errors (transport,");
    w.write("/// serialization, unknown, or another operation's error).");
    boolean withOrdering = errors.stream().allMatch(e -> context.cppSymbols().orderable(e));
    TypeGenerators.emitTaggedVariant(
        w,
        listingName,
        members,
        /* withFactories= */ false,
        withOrdering,
        /* redactBody= */ false,
        "/// True when the error is none of this operation's modeled errors.",
        "/// Name of the engaged member, \"(empty)\" when none matched.",
        () -> writeFromError(w, listingName, errors));
  }

  private void writeFromError(CppWriter w, String listingName, List<StructureShape> errors) {
    w.write("/// Matches `error` against this operation's modeled errors. An engaged");
    w.write("/// member carries the deserialized error detail, default-initialized when");
    w.write("/// the error arrived without one.");
    w.openBlock("static $L FromError(const opal::Error& error) {", listingName);
    w.write("$L result;", listingName);
    w.write("if (error.kind() != opal::ErrorKind::kModeled) return result;");
    for (int i = 0; i < errors.size(); ++i) {
      StructureShape error = errors.get(i);
      String typeName = context.cppSymbols().toSymbol(error).getName();
      w.openBlock("if (error.code() == $S) {", error.getId().getName());
      w.write("const auto* detail = error.detail<$L>();", typeName);
      w.write("result.value_.emplace<$L>(detail != nullptr ? *detail : $L{});", i + 1, typeName);
      w.write("return result;");
      w.closeBlock("}");
    }
    w.write("return result;");
    w.closeBlock("}");
  }

  /**
   * The listing's synthetic name lives beside the model's own types, so a modeled shape with the
   * same C++ name in this namespace must fail loudly (the plural already dodges the
   * operation-named-error convention, e.g. DescribeSink vs DescribeSinkError). The shared check
   * (TypeGenerators) also guards the generated stream aliases.
   */
  private void requireNoModelCollision(OperationShape operation, String listingName) {
    TypeGenerators.requireNoModelCollision(
        context, service, operation, listingName, "error listing");
  }

  private String errorsName(OperationShape operation) {
    return CppReservedWords.escape(operation.getId().getName()) + "Errors";
  }

  /**
   * Resolved pagination for the operation, when the generator supports it: top-level string
   * input/output token members (nested output-token paths and non-string tokens are skipped).
   */
  private java.util.Optional<software.amazon.smithy.model.knowledge.PaginationInfo> pagination(
      OperationShape operation) {
    return software.amazon.smithy.model.knowledge.PaginatedIndex.of(context.model())
        .getPaginationInfo(service, operation)
        .filter(
            info ->
                info.getOutputTokenMemberPath().size() == 1
                    && context
                        .model()
                        .expectShape(info.getInputTokenMember().getTarget())
                        .isStringShape()
                    && context
                        .model()
                        .expectShape(info.getOutputTokenMemberPath().get(0).getTarget())
                        .isStringShape());
  }

  private String paginatorName(OperationShape operation) {
    return CppReservedWords.escape(operation.getId().getName()) + "Paginator";
  }

  /**
   * Auth from the service's traits; a null provider leaves the request anonymous. Static so the
   * protocols' streaming operation bodies attach the same auth to the upgrade request (their local
   * is also named {@code request}, with .headers/.target members) — one emission, no drift.
   */
  static void writeAuth(CppWriter w, ServiceShape service) {
    if (service.hasTrait(software.amazon.smithy.model.traits.HttpBearerAuthTrait.class)) {
      w.write("// @httpBearerAuth: attach the configured token (fetched per request).");
      w.openBlock("if (config_.bearer_token) {");
      w.write("request.headers.Set(\"authorization\", \"Bearer \" + config_.bearer_token());");
      w.closeBlock("}");
    }
    service
        .getTrait(software.amazon.smithy.model.traits.HttpApiKeyAuthTrait.class)
        .ifPresent(
            trait -> {
              w.write("// @httpApiKeyAuth: attach the configured key where the model binds it.");
              w.openBlock("if (config_.api_key) {");
              if (trait.getIn()
                  == software.amazon.smithy.model.traits.HttpApiKeyAuthTrait.Location.HEADER) {
                String prefix = trait.getScheme().map(scheme -> scheme + " ").orElse("");
                if (prefix.isEmpty()) {
                  w.write("request.headers.Set($S, config_.api_key());", trait.getName());
                } else {
                  w.write(
                      "request.headers.Set($S, $S + config_.api_key());", trait.getName(), prefix);
                }
              } else {
                w.write(
                    "request.target += request.target.find('?') == std::string::npos "
                        + "? \"?\" : \"&\";");
                w.write(
                    "request.target += \"$L=\" + "
                        + "opal::http::EncodeQueryComponent(config_.api_key());",
                    trait.getName());
              }
              w.closeBlock("}");
            });
  }

  private void writeSource(CppWriter w) {
    for (String include : ProtocolSupport.sharedClientIncludes(context)) {
      w.addInclude(include);
    }
    for (String include : protocol.clientIncludes()) {
      w.addInclude(include);
    }
    String name = clientName();

    ProtocolSupport.openHelpersNamespace(w);
    protocol.writeClientHelpers(w, context);
    ProtocolSupport.writeOperationErrorParsers(w, context, service, protocol, operations());
    if (!streamingOperations().isEmpty()) {
      EventStreamCodeGen.writeClientStreamHelpers(
          w, context, service, protocol, streamingOperations(), name);
    }
    ProtocolSupport.closeHelpersNamespace(w);

    w.openBlock("opal::Outcome<$L> $L::Create(opal::ClientConfig config) {", name, name);
    w.write("std::shared_ptr<opal::http::HttpClient> transport = config.http_client;");
    w.write("std::string prefix;");
    w.openBlock("if (!config.endpoint.empty()) {");
    w.write("auto endpoint = opal::http::ParseEndpoint(config.endpoint);");
    w.write("if (!endpoint) return std::move(endpoint).error();");
    w.write("prefix = endpoint->path_prefix;");
    w.openBlock("if (transport == nullptr) {");
    w.write("// The built-in socket transport is plaintext-only; https needs a");
    w.write("// TLS-capable transport (e.g. opal::http::BeastHttpClient).");
    w.openBlock("if (endpoint->tls()) {");
    w.write(
        "return opal::Error::Validation($S);",
        name
            + ": https endpoints need a TLS-capable transport"
            + " (set config.http_client, e.g. opal::http::BeastHttpClient::FromConfig)");
    w.closeBlock("}");
    w.write(
        "transport = std::make_shared<opal::http::SocketHttpClient>(endpoint->host, "
            + "endpoint->port, config.request_timeout_ms);");
    w.closeBlock("}");
    w.closeBlock("}");
    w.openBlock("if (transport == nullptr) {");
    w.write(
        "return opal::Error::Validation($S);",
        name + ": config needs an endpoint or an http_client");
    w.closeBlock("}");
    w.write("return $L(std::move(config), std::move(transport), std::move(prefix));", name);
    w.closeBlock("}");
    w.write("");

    w.openBlock(
        "$L::$L(opal::ClientConfig config, "
            + "std::shared_ptr<opal::http::HttpClient> transport, std::string path_prefix)",
        name,
        name);
    w.write(": config_(std::move(config)),");
    w.write("  transport_(std::move(transport)),");
    w.write("  path_prefix_(std::move(path_prefix)) {}");
    w.dedent();
    w.write("");

    w.openBlock(
        "opal::Outcome<opal::http::HttpResponse> $L::Send("
            + "opal::http::HttpRequest request) const {",
        name);
    w.write("// Operations with a non-document response payload set their own accept.");
    w.write(
        "if (!request.headers.Get(\"accept\").has_value()) "
            + "request.headers.Set(\"accept\", $S);",
        protocol.contentType());
    w.write("request.headers.Set(\"user-agent\", config_.user_agent);");
    writeAuth(w, service);
    w.openBlock("if (!request.body.empty()) {");
    w.write("request.headers.Set(\"content-length\", std::to_string(request.body.size()));");
    w.closeBlock("}");
    w.write(
        "return opal::SendWithRetries(*transport_, request, config_.retry, "
            + "config_.interceptors);");
    w.closeBlock("}");
    w.write("");

    for (OperationShape operation : operations()) {
      StructureShape input = ProtocolSupport.inputShape(context, operation);
      String inputType = context.cppSymbols().toSymbol(input).getName();
      boolean streaming = EventStreamCodeGen.streaming(context.model(), operation);
      String outputType =
          streaming
              ? EventStreamCodeGen.clientStreamAlias(operation)
              : context
                  .cppSymbols()
                  .toSymbol(ProtocolSupport.outputShape(context, operation))
                  .getName();
      w.openBlock(
          "opal::Outcome<$L> $L::$L(const $L& input) const {",
          outputType,
          name,
          CppReservedWords.escape(operation.getId().getName()),
          inputType);
      if (input.members().isEmpty()) {
        w.write("(void)input;");
      }
      if (streaming) {
        protocol.writeStreamingOperationBody(w, context, service, operation);
      } else {
        protocol.writeOperationBody(w, context, service, operation);
      }
      w.closeBlock("}");
      w.write("");
    }

    for (OperationShape operation : operations()) {
      pagination(operation).ifPresent(info -> writePaginator(w, operation, info));
    }
  }

  private void writePaginator(
      CppWriter w,
      OperationShape operation,
      software.amazon.smithy.model.knowledge.PaginationInfo info) {
    String name = clientName();
    String pager = paginatorName(operation);
    String opName = CppReservedWords.escape(operation.getId().getName());
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    String inputType = context.cppSymbols().toSymbol(input).getName();
    String outputType =
        context.cppSymbols().toSymbol(ProtocolSupport.outputShape(context, operation)).getName();
    String inToken = context.cppSymbols().toMemberName(info.getInputTokenMember());
    String outToken = context.cppSymbols().toMemberName(info.getOutputTokenMemberPath().get(0));

    w.openBlock("$L $L::Paginate$L($L input) const {", pager, name, opName, inputType);
    w.write("return $L(*this, std::move(input));", pager);
    w.closeBlock("}");
    w.write("");

    // The output token is always optional: Smithy's paginated validator
    // rejects @required output tokens at assembly (pinned by
    // ConditionalWiringCoverageTest), so there is no plain-member arm here.
    w.openBlock("opal::Outcome<std::optional<$L>> $L::Next() {", outputType, pager);
    w.write("if (done_) return std::optional<$L>();", outputType);
    w.write("auto page = client_.$L(input_);", opName);
    w.openBlock("if (!page) {");
    w.write("done_ = true;");
    w.write("return std::move(page).error();");
    w.closeBlock("}");
    w.openBlock("if (!page->$1L.has_value() || page->$1L->empty()) {", outToken);
    w.write("done_ = true;");
    w.dedent();
    w.write("} else {");
    w.indent();
    w.write("input_.$L = *page->$L;", inToken, outToken);
    w.closeBlock("}");
    w.write("return std::optional<$L>(std::move(*page));", outputType);
    w.closeBlock("}");
    w.write("");
  }
}
