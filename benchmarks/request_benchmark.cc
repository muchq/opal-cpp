// End-to-end request throughput (PLAN Phase 7 "Performance"): generated
// client against generated server over the in-memory loopback, one benchmark
// per protocol, so the numbers isolate serde + protocol + router cost from
// the network. Run with
//   bazel run -c opt //benchmarks:request_benchmark

#include <benchmark/benchmark.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "example/roundtrip/jsonrpc/client.h"
#include "example/roundtrip/jsonrpc/server.h"
#include "example/roundtrip/rest/client.h"
#include "example/roundtrip/rest/server.h"
#include "example/roundtrip/rpc/client.h"
#include "example/roundtrip/rpc/server.h"
#include "smithy/client/config.h"
#include "smithy/http/loopback.h"

namespace {

// The same nested input shape for every protocol variant.
template <typename Input, typename Nested>
Input MakeInput() {
  Input input;
  input.sinkId = "bench1";
  typename std::decay_t<decltype(*input.sink)> sink{};
  sink.name = "benchmark-sink";
  sink.medium = 123456;
  sink.names = std::vector<std::string>{"alpha", "beta", "gamma"};
  sink.attributes = std::map<std::string, std::string>{{"env", "bench"}, {"tier", "gold"}};
  sink.nested = Nested{.label = "nested", .depth = 4};
  input.sink = std::move(sink);
  return input;
}

// Echo handlers: answer with a fixed minimal output so the measured work is
// the request/response pipeline, not handler logic.

class RestHandler final : public example::roundtrip::rest::RoundTripRestHandler {
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

class RpcHandler final : public example::roundtrip::rpc::RoundTripRpcHandler {
 public:
  opal::Outcome<example::roundtrip::rpc::PutSinkRpcOutput> PutSinkRpc(
      const example::roundtrip::rpc::PutSinkRpcInput& input,
      const opal::server::RequestContext&) override {
    return example::roundtrip::rpc::PutSinkRpcOutput{.sinkId = input.sinkId, .sink = input.sink};
  }
  opal::Outcome<example::roundtrip::rpc::PingOutput> Ping(
      const example::roundtrip::rpc::PingInput&, const opal::server::RequestContext&) override {
    return example::roundtrip::rpc::PingOutput{};
  }
};

class JsonRpcHandler final : public example::roundtrip::jsonrpc::RoundTripJsonRpcHandler {
 public:
  opal::Outcome<example::roundtrip::jsonrpc::PutSinkRpcOutput> PutSinkRpc(
      const example::roundtrip::jsonrpc::PutSinkRpcInput& input,
      const opal::server::RequestContext&) override {
    return example::roundtrip::jsonrpc::PutSinkRpcOutput{.sinkId = input.sinkId,
                                                         .sink = input.sink};
  }
};

template <typename Client, typename Server, typename Handler>
Client MakeLoopbackClient() {
  // Handler() owns the router, so the Server object itself may go away.
  Server server(std::make_shared<Handler>());
  auto loopback = std::make_shared<opal::http::Loopback>();
  (void)loopback->Start(server.Handler());
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.http_client = std::move(loopback);
  return *Client::Create(std::move(config));
}

void BM_SimpleRestJsonRoundTrip(benchmark::State& state) {
  auto client = MakeLoopbackClient<example::roundtrip::rest::RoundTripRestClient,
                                   example::roundtrip::rest::RoundTripRestServer, RestHandler>();
  const auto input =
      MakeInput<example::roundtrip::rest::PutSinkInput, example::roundtrip::rest::NestedConfig>();
  for (auto _ : state) {
    benchmark::DoNotOptimize(client.PutSink(input));
  }
}
BENCHMARK(BM_SimpleRestJsonRoundTrip);

void BM_Rpcv2CborRoundTrip(benchmark::State& state) {
  auto client = MakeLoopbackClient<example::roundtrip::rpc::RoundTripRpcClient,
                                   example::roundtrip::rpc::RoundTripRpcServer, RpcHandler>();
  const auto input =
      MakeInput<example::roundtrip::rpc::PutSinkRpcInput, example::roundtrip::rpc::NestedConfig>();
  for (auto _ : state) {
    benchmark::DoNotOptimize(client.PutSinkRpc(input));
  }
}
BENCHMARK(BM_Rpcv2CborRoundTrip);

void BM_JsonRpc2RoundTrip(benchmark::State& state) {
  auto client =
      MakeLoopbackClient<example::roundtrip::jsonrpc::RoundTripJsonRpcClient,
                         example::roundtrip::jsonrpc::RoundTripJsonRpcServer, JsonRpcHandler>();
  const auto input = MakeInput<example::roundtrip::jsonrpc::PutSinkRpcInput,
                               example::roundtrip::jsonrpc::NestedConfig>();
  for (auto _ : state) {
    benchmark::DoNotOptimize(client.PutSinkRpc(input));
  }
}
BENCHMARK(BM_JsonRpc2RoundTrip);

}  // namespace

BENCHMARK_MAIN();
