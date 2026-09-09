// Transport round trips over real TCP (PLAN Phase 7 "Performance"): the same
// generated PutSink call as request_benchmark, but through the kernel instead
// of the in-memory loopback — the dependency-free socket transport
// (connection per request), BeastHttpClient over BeastServerTransport with
// keep-alive reuse, and the same pair with TLS both directions. Comparing
// against BM_SimpleRestJsonRoundTrip isolates what the network stack and TLS
// add. Run with
//   bazel run -c opt //benchmarks:beast_benchmark

#include <benchmark/benchmark.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "example/roundtrip/rest/client.h"
#include "example/roundtrip/rest/server.h"
#include "opal/client/config.h"
#include "opal/http/beast_transport.h"
#include "opal/http/socket_transport.h"
#include "opal/testing/tls_test_identity.h"

namespace {

using opal::testing::kTestCertificatePem;
using opal::testing::kTestPrivateKeyPem;

// The same input request_benchmark sends, so the numbers are comparable.
example::roundtrip::rest::PutSinkInput MakeInput() {
  example::roundtrip::rest::PutSinkInput input;
  input.sinkId = "bench1";
  example::roundtrip::rest::KitchenSink sink;
  sink.name = "benchmark-sink";
  sink.medium = 123456;
  sink.names = std::vector<std::string>{"alpha", "beta", "gamma"};
  sink.attributes = std::map<std::string, std::string>{{"env", "bench"}, {"tier", "gold"}};
  sink.nested = example::roundtrip::rest::NestedConfig{.label = "nested", .depth = 4};
  input.sink = std::move(sink);
  return input;
}

class EchoHandler final : public example::roundtrip::rest::RoundTripRestHandler {
 public:
  opal::Outcome<example::roundtrip::rest::PutSinkOutput> PutSink(
      const example::roundtrip::rest::PutSinkInput& input,
      const opal::server::RequestContext&) override {
    return example::roundtrip::rest::PutSinkOutput{.sinkId = input.sinkId, .sink = input.sink};
  }
  opal::Outcome<example::roundtrip::rest::UploadAttachmentOutput> UploadAttachment(
      const example::roundtrip::rest::UploadAttachmentInput&,
      const opal::server::RequestContext&) override {
    return example::roundtrip::rest::UploadAttachmentOutput{};
  }
  opal::Outcome<example::roundtrip::rest::DescribeSinkOutput> DescribeSink(
      const example::roundtrip::rest::DescribeSinkInput&,
      const opal::server::RequestContext&) override {
    return example::roundtrip::rest::DescribeSinkOutput{};
  }
};

example::roundtrip::rest::RoundTripRestClient MakeClient(
    std::string endpoint, std::shared_ptr<opal::http::HttpClient> transport) {
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.endpoint = std::move(endpoint);
  config.http_client = std::move(transport);
  return *example::roundtrip::rest::RoundTripRestClient::Create(std::move(config));
}

void RunLoop(benchmark::State& state, example::roundtrip::rest::RoundTripRestClient& client) {
  const auto input = MakeInput();
  for (auto _ : state) {
    auto outcome = client.PutSink(input);
    if (!outcome.ok()) {
      state.SkipWithError(outcome.error().message().c_str());
      return;
    }
    benchmark::DoNotOptimize(outcome);
  }
}

// Baseline: the dependency-free socket transport, one connection per request.
void BM_SocketRoundTrip(benchmark::State& state) {
  example::roundtrip::rest::RoundTripRestServer server(std::make_shared<EchoHandler>());
  opal::http::SocketHttpServer transport;
  if (!transport.Start(server.Handler()).ok()) {
    state.SkipWithError("socket server failed to start");
    return;
  }
  const std::string origin = "http://127.0.0.1:" + std::to_string(transport.port());
  auto client = MakeClient(
      origin, std::make_shared<opal::http::SocketHttpClient>("127.0.0.1", transport.port()));
  RunLoop(state, client);
  transport.Stop();
}
BENCHMARK(BM_SocketRoundTrip);

void BM_BeastRoundTrip(benchmark::State& state) {
  example::roundtrip::rest::RoundTripRestServer server(std::make_shared<EchoHandler>());
  opal::http::BeastServerTransport transport({.port = 0, .threads = 2});
  if (!transport.Start(server.Handler()).ok()) {
    state.SkipWithError("beast server failed to start");
    return;
  }
  const std::string origin = "http://127.0.0.1:" + std::to_string(transport.port());
  auto client = MakeClient(
      origin, std::make_shared<opal::http::BeastHttpClient>(opal::http::BeastHttpClient::Options{
                  .host = "127.0.0.1", .port = transport.port()}));
  RunLoop(state, client);
  transport.Stop();
}
BENCHMARK(BM_BeastRoundTrip);

void BM_BeastTlsRoundTrip(benchmark::State& state) {
  example::roundtrip::rest::RoundTripRestServer server(std::make_shared<EchoHandler>());
  opal::http::BeastServerTransport transport({.port = 0,
                                              .threads = 2,
                                              .tls_certificate_chain_pem = kTestCertificatePem,
                                              .tls_private_key_pem = kTestPrivateKeyPem});
  if (!transport.Start(server.Handler()).ok()) {
    state.SkipWithError("beast tls server failed to start");
    return;
  }
  const std::string origin = "https://127.0.0.1:" + std::to_string(transport.port());
  auto client = MakeClient(
      origin, std::make_shared<opal::http::BeastHttpClient>(opal::http::BeastHttpClient::Options{
                  .host = "127.0.0.1",
                  .port = transport.port(),
                  .tls = true,
                  .tls_options = {.ca_pem = kTestCertificatePem}}));
  RunLoop(state, client);
  transport.Stop();
}
BENCHMARK(BM_BeastTlsRoundTrip);

}  // namespace

BENCHMARK_MAIN();
