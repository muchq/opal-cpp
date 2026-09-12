package io.smithycpp.codegen;

import software.amazon.smithy.codegen.core.SymbolProvider;
import software.amazon.smithy.codegen.core.WriterDelegator;
import software.amazon.smithy.codegen.core.directed.CreateContextDirective;
import software.amazon.smithy.codegen.core.directed.CreateSymbolProviderDirective;
import software.amazon.smithy.codegen.core.directed.DirectedCodegen;
import software.amazon.smithy.codegen.core.directed.GenerateEnumDirective;
import software.amazon.smithy.codegen.core.directed.GenerateErrorDirective;
import software.amazon.smithy.codegen.core.directed.GenerateIntEnumDirective;
import software.amazon.smithy.codegen.core.directed.GenerateServiceDirective;
import software.amazon.smithy.codegen.core.directed.GenerateStructureDirective;
import software.amazon.smithy.codegen.core.directed.GenerateUnionDirective;

/**
 * Directs C++ code generation. Phase 2 scope: data types only (structures, errors, unions, enums),
 * plus the generated module's BUILD file. Shapes arrive in topological order, so the single types.h
 * needs no forward declarations.
 */
public final class DirectedCppCodegen
    implements DirectedCodegen<CppContext, CppSettings, CppIntegration> {

  @Override
  public SymbolProvider createSymbolProvider(CreateSymbolProviderDirective<CppSettings> directive) {
    return new CppSymbolProvider(directive.model(), directive.settings());
  }

  @Override
  public CppContext createContext(CreateContextDirective<CppSettings, CppIntegration> directive) {
    return new CppContext(
        directive.model(),
        directive.settings(),
        directive.symbolProvider(),
        directive.fileManifest(),
        new WriterDelegator<>(
            directive.fileManifest(),
            directive.symbolProvider(),
            new CppWriter.Factory(directive.settings())),
        directive.integrations(),
        new CppSymbolProvider(directive.model(), directive.settings()));
  }

  @Override
  public void generateService(GenerateServiceDirective<CppContext, CppSettings> directive) {
    software.amazon.smithy.model.shapes.ServiceShape service = directive.shape();
    ProtocolGenerator protocol = resolveProtocol(service);

    SerdeGenerator serdeGenerator =
        new SerdeGenerator(directive.context(), protocol != null && protocol.usesJsonName());
    boolean hasSerde =
        !directive.settings().mode().equals("types") && !serdeGenerator.serdeShapes().isEmpty();
    if (hasSerde) {
      serdeGenerator.run();
    }

    boolean hasClient = false;
    boolean hasServer = false;
    if (protocol != null && !directive.settings().mode().equals("types")) {
      ClientGenerator clientGenerator = new ClientGenerator(directive.context(), service, protocol);
      java.util.List<software.amazon.smithy.model.shapes.OperationShape> operations =
          clientGenerator.operations();
      if (!operations.isEmpty()) {
        // Event-stream scope checks (ADR-0016) fail generation with a named
        // diagnostic before any streaming code is emitted.
        EventStreamCodeGen.validate(directive.context(), service, protocol, operations);
        // The streaming-payload scope check (#213 slice 2), same posture: a
        // named diagnostic before any client is emitted.
        HttpBindingCodeGen.validateStreamingPayloads(directive.context(), protocol, operations);
        if (directive.settings().generateClient()) {
          clientGenerator.run();
          hasClient = true;
        }
        if (directive.settings().generateServer()) {
          new ServerGenerator(directive.context(), service, protocol, operations).run();
          hasServer = true;
        }
        // The generated test suites drive the client against the server, so
        // they only exist for mode=both (the fixture/protocol-test pipelines).
        if (directive.settings().testsPackage() != null && hasClient && hasServer) {
          new SmokeTestGenerator(directive.context(), service, operations).run();
          boolean hasProtocolTests =
              operations.stream()
                  .anyMatch(
                      op ->
                          op.hasTrait(
                                  software.amazon.smithy.protocoltests.traits.HttpRequestTestsTrait
                                      .class)
                              || op.hasTrait(
                                  software.amazon.smithy.protocoltests.traits.HttpResponseTestsTrait
                                      .class));
          boolean hasMalformedTests =
              directive.settings().malformedTests()
                  && operations.stream()
                      .anyMatch(
                          op ->
                              op.hasTrait(
                                  software.amazon.smithy.protocoltests.traits
                                      .HttpMalformedRequestTestsTrait.class));
          if (hasProtocolTests || hasMalformedTests) {
            new ProtocolTestGenerator(
                    directive.context(),
                    service,
                    protocol,
                    operations,
                    hasProtocolTests,
                    hasMalformedTests)
                .run();
          }
          boolean hasIntegrationTests = directive.settings().integrationTests();
          if (hasIntegrationTests) {
            new IntegrationTestGenerator(directive.context(), service, protocol, operations).run();
          }
          TestsBuildFileGenerator.run(
              directive.context(), hasProtocolTests, hasMalformedTests, hasIntegrationTests);
        }
      }
    }
    if (directive.settings().emitBuildFile()) {
      // Scoped to the service closure, not the whole model: a multi-service
      // model (roundtrip) must not link :compression into every service's
      // BUILD because one sibling service compresses.
      boolean hasCompression =
          protocol != null
              && ProtocolSupport.containedOperations(directive.context().model(), service).stream()
                  .anyMatch(ProtocolSupport::gzipCompressed);
      // Same closure scoping for the event-stream deps (ADR-0016): only a
      // streaming service links :eventstream (and, client-side, :http_beast).
      boolean hasStreaming =
          protocol != null
              && ProtocolSupport.containedOperations(directive.context().model(), service).stream()
                  .anyMatch(op -> EventStreamCodeGen.streaming(directive.context().model(), op));
      BuildFileGenerator.run(
          directive.context(),
          protocol,
          hasClient,
          hasSerde,
          hasServer,
          hasCompression,
          hasStreaming);
    }
  }

  /** Null when the service declares no supported protocol (types+serde still generate). */
  private static ProtocolGenerator resolveProtocol(
      software.amazon.smithy.model.shapes.ServiceShape service) {
    if (service.hasTrait(HttpJsonBindingProtocol.SIMPLE_REST_JSON_TRAIT)) {
      return HttpJsonBindingProtocol.simpleRestJson();
    }
    if (service.hasTrait(software.amazon.smithy.protocol.traits.Rpcv2CborTrait.class)) {
      return new Rpcv2CborProtocol();
    }
    if (service.hasTrait(JsonRpc2Protocol.TRAIT)) {
      return new JsonRpc2Protocol();
    }
    return null;
  }

  @Override
  public void generateStructure(GenerateStructureDirective<CppContext, CppSettings> directive) {
    if (directive.shape().getId().toString().equals("smithy.api#Unit")) {
      return; // Maps to the runtime's opal::Unit; nothing to declare.
    }
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateStructure(directive.shape()));
  }

  @Override
  public void generateError(GenerateErrorDirective<CppContext, CppSettings> directive) {
    // Errors are plain structures until protocol-aware error handling lands in Phase 3.
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateStructure(directive.shape()));
  }

  @Override
  public void generateUnion(GenerateUnionDirective<CppContext, CppSettings> directive) {
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer).generateUnion(directive.shape()));
  }

  @Override
  public void generateEnumShape(GenerateEnumDirective<CppContext, CppSettings> directive) {
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateEnum(directive.shape().asEnumShape().orElseThrow()));
  }

  @Override
  public void generateIntEnumShape(GenerateIntEnumDirective<CppContext, CppSettings> directive) {
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateIntEnum(
                        (software.amazon.smithy.model.shapes.IntEnumShape) directive.shape()));
  }
}
