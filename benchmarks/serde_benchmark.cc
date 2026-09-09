// Serde throughput (PLAN Phase 7 "Performance"): the kitchen-sink shape
// through the Document pivot and both wire codecs. Run with
//   bazel run -c opt //benchmarks:serde_benchmark

#include <benchmark/benchmark.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "example/roundtrip/rest/serde.h"
#include "example/roundtrip/rest/types.h"
#include "opal/cbor/cbor.h"
#include "opal/core/blob.h"
#include "opal/core/document.h"
#include "opal/json/json.h"

namespace {

using example::roundtrip::rest::KitchenSink;
using example::roundtrip::rest::NestedConfig;
using example::roundtrip::rest::Priority;
using example::roundtrip::rest::SinkChoice;
using example::roundtrip::rest::Weight;

KitchenSink MakeSink() {
  KitchenSink sink;
  sink.name = "benchmark-sink";
  sink.flag = true;
  sink.tiny = 1;
  sink.small = 512;
  sink.medium = 123456;
  sink.big = 9876543210LL;
  sink.ratio = 0.5F;
  sink.precise = 3.14159265358979;
  sink.blob = opal::Blob::FromString("0123456789abcdef0123456789abcdef");
  sink.priority = Priority::Value::kHigh;
  sink.weight = Weight::kHeavy;
  sink.dateTime = opal::Timestamp::FromEpochMilliseconds(1515531081000LL);
  sink.httpDate = opal::Timestamp::FromEpochMilliseconds(1515531081000LL);
  sink.epoch = opal::Timestamp::FromEpochMilliseconds(1515531081000LL);
  sink.names = std::vector<std::string>{"alpha", "beta", "gamma", "delta", "epsilon"};
  sink.uniqueNames = std::vector<std::string>{"one", "two", "three"};
  sink.sparseNumbers =
      std::vector<std::optional<std::int32_t>>{1, std::nullopt, 3, std::nullopt, 5};
  sink.attributes =
      std::map<std::string, std::string>{{"env", "bench"}, {"region", "local"}, {"tier", "gold"}};
  sink.nested = NestedConfig{.label = "nested", .depth = 4};
  sink.choice = SinkChoice::FromNested(NestedConfig{.label = "choice", .depth = 2});
  return sink;
}

void BM_SerializeToDocument(benchmark::State& state) {
  const KitchenSink sink = MakeSink();
  for (auto _ : state) {
    benchmark::DoNotOptimize(example::roundtrip::rest::SerializeKitchenSink(sink));
  }
}
BENCHMARK(BM_SerializeToDocument);

void BM_DeserializeFromDocument(benchmark::State& state) {
  const opal::Document doc = example::roundtrip::rest::SerializeKitchenSink(MakeSink());
  for (auto _ : state) {
    benchmark::DoNotOptimize(example::roundtrip::rest::DeserializeKitchenSink(doc));
  }
}
BENCHMARK(BM_DeserializeFromDocument);

void BM_JsonEncode(benchmark::State& state) {
  const opal::Document doc = example::roundtrip::rest::SerializeKitchenSink(MakeSink());
  std::size_t bytes = 0;
  for (auto _ : state) {
    std::string text = opal::json::Encode(doc);
    bytes += text.size();
    benchmark::DoNotOptimize(text);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}
BENCHMARK(BM_JsonEncode);

void BM_JsonDecode(benchmark::State& state) {
  const std::string text =
      opal::json::Encode(example::roundtrip::rest::SerializeKitchenSink(MakeSink()));
  std::size_t bytes = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(opal::json::Decode(text));
    bytes += text.size();
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}
BENCHMARK(BM_JsonDecode);

void BM_CborEncode(benchmark::State& state) {
  const opal::Document doc = example::roundtrip::rest::SerializeKitchenSink(MakeSink());
  std::size_t bytes = 0;
  for (auto _ : state) {
    opal::Blob wire = opal::cbor::Encode(doc);
    bytes += wire.size();
    benchmark::DoNotOptimize(wire);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}
BENCHMARK(BM_CborEncode);

void BM_CborDecode(benchmark::State& state) {
  const opal::Blob wire =
      opal::cbor::Encode(example::roundtrip::rest::SerializeKitchenSink(MakeSink()));
  std::size_t bytes = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(opal::cbor::Decode(wire));
    bytes += wire.size();
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}
BENCHMARK(BM_CborDecode);

void BM_FullPivotRoundTripJson(benchmark::State& state) {
  const KitchenSink sink = MakeSink();
  for (auto _ : state) {
    const std::string text =
        opal::json::Encode(example::roundtrip::rest::SerializeKitchenSink(sink));
    auto doc = opal::json::Decode(text);
    benchmark::DoNotOptimize(example::roundtrip::rest::DeserializeKitchenSink(*doc));
  }
}
BENCHMARK(BM_FullPivotRoundTripJson);

}  // namespace

BENCHMARK_MAIN();
