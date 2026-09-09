// The jsonRpc2 stream wire end to end over the in-memory pair (ADR-0023):
// the generated client and generated server round-trip typed events through
// the JSON-RPC envelopes on both serve seams; the modeled Overflow arrives
// typed as the terminal error; and — because the translation runs above the
// socket — the same fixture pins the ACTUAL wire text a vanilla JSON-RPC
// peer (or a browser with JSON.parse) exchanges, raw headerless messages on
// an unserved pair end. The Beast half of the same flows lives in
// stream_e2e_beast_test.cc.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "accumulate_handler.h"
#include "example/calculator/client.h"
#include "example/calculator/server.h"
#include "smithy/core/error.h"
#include "smithy/http/websocket_pair.h"
#include "stream_test_fixture.h"

namespace example::calculator {
namespace {

// One raw wire frame: a headerless message whose payload is the envelope
// text — exactly what a browser's WebSocket send() puts on a real wire.
opal::eventstream::Message RawText(std::string text) {
  opal::eventstream::Message message;
  message.payload = opal::Blob::FromString(std::move(text));
  return message;
}

class AccumulateEndToEndTest : public StreamTestFixture {
 protected:
  // The shared happy path both seams must serve identically.
  void RunningTotalsRoundTrip() {
    AccumulateInput input;
    input.start = 10.0;
    auto stream = client_->Accumulate(input);
    ASSERT_TRUE(stream.ok()) << stream.error().message();
    EXPECT_EQ(last_dialed_target_, "/");  // the shared endpoint, not a per-op URI

    ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 5})).ok());
    auto first = stream->Receive();
    ASSERT_TRUE(first.ok() && first->has_value());
    EXPECT_EQ((**first).as_total().value, 15.0);

    ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 7})).ok());
    auto second = stream->Receive();
    ASSERT_TRUE(second.ok() && second->has_value());
    EXPECT_EQ((**second).as_total().value, 22.0);

    // The "=" key: the handler completes cleanly, the terminal result
    // envelope crosses, and the wrapper surfaces it as the stream's end.
    ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 0})).ok());
    auto end = stream->Receive();
    ASSERT_TRUE(end.ok()) << end.error().message();
    EXPECT_FALSE(end->has_value());

    // The end is stable: the server's close follows its terminal, and a
    // Receive past the end reports the same closed stream, no hang.
    auto after = stream->Receive();
    ASSERT_TRUE(after.ok()) << after.error().message();
    EXPECT_FALSE(after->has_value());
  }

  void OverflowSurfacesTyped() {
    AccumulateInput input;
    input.start = 90.0;
    auto stream = client_->Accumulate(input);
    ASSERT_TRUE(stream.ok()) << stream.error().message();

    ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 20})).ok());
    auto outcome = stream->Receive();
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
    EXPECT_EQ(outcome.error().code(), "Overflow");
    const Overflow* detail = outcome.error().detail<Overflow>();
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->limit, kAccumulateLimit);
  }
};

TEST_F(AccumulateEndToEndTest, RunningTotalsRoundTripOnTheBlockingSeam) {
  StartWith(std::make_shared<AccumulatingCalculator>());
  RunningTotalsRoundTrip();
}

TEST_F(AccumulateEndToEndTest, RunningTotalsRoundTripOnTheSessionSeamWithoutThreads) {
  StartWithAsync(std::make_shared<AsyncAccumulatingCalculator>());
  RunningTotalsRoundTrip();
  EXPECT_TRUE(threads_.empty());  // ADR-0021: no thread parked per session
}

TEST_F(AccumulateEndToEndTest, TheModeledOverflowIsTheTypedTerminalError) {
  StartWith(std::make_shared<AccumulatingCalculator>());
  OverflowSurfacesTyped();
}

TEST_F(AccumulateEndToEndTest, TheModeledOverflowIsTypedOnTheSessionSeamToo) {
  StartWithAsync(std::make_shared<AsyncAccumulatingCalculator>());
  OverflowSurfacesTyped();
}

TEST_F(AccumulateEndToEndTest, UnaryAndStreamingShareTheGeneratedServer) {
  StartWith(std::make_shared<AccumulatingCalculator>());
  auto sum = client_->Add(AddInput{.a = 2, .b = 3});
  ASSERT_TRUE(sum.ok()) << sum.error().message();
  EXPECT_EQ(sum->sum, 5.0);
  RunningTotalsRoundTrip();
}

TEST_F(AccumulateEndToEndTest, TheWireIsPlainJsonRpcTextEndToEnd) {
  // The vanilla-peer story (issue #123's acceptance): a client that speaks
  // nothing but JSON-RPC 2.0 — no smithy, no codec — opens with a request
  // envelope, streams notifications both ways with the id echoed inside
  // params, and observes a well-formed response for its call, then the
  // close. Byte-pinned: the runtime's JSON output is deterministic.
  StartWithAsync(std::make_shared<AsyncAccumulatingCalculator>());
  auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
  sessions_.push_back(near);
  sessions_.push_back(far);
  Serve("/", far);

  ASSERT_TRUE(
      near->Send(RawText(R"({"jsonrpc":"2.0","method":"Accumulate","params":{"start":1},"id":7})"))
          .ok());
  ASSERT_TRUE(
      near
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"add","params":{"id":7,"payload":{"value":0.5}}})"))
          .ok());
  auto total = near->Receive();
  ASSERT_TRUE(total.ok() && total->has_value());
  EXPECT_TRUE((**total).headers.empty());
  // A non-integral total on purpose: 1.5 has one canonical rendering,
  // so the byte pin doesn't ride the encoder's integral-double spelling.
  EXPECT_EQ((**total).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"total","params":{"id":7,"payload":{"value":1.5}}})");

  ASSERT_TRUE(
      near->Send(RawText(
                     R"({"jsonrpc":"2.0","method":"add","params":{"id":7,"payload":{"value":0}}})"))
          .ok());
  auto terminal = near->Receive();
  ASSERT_TRUE(terminal.ok() && terminal->has_value());
  EXPECT_EQ((**terminal).payload.ToString(), R"({"id":7,"jsonrpc":"2.0","result":{}})");
  auto end = near->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());  // the server closes behind its terminal
}

TEST_F(AccumulateEndToEndTest, AUnaryMethodOnTheStreamEndpointIsUnknown) {
  // The stream endpoint dispatches streaming operations only; a unary
  // method name earns the reserved -32601 as a terminal error, then close.
  StartWithAsync(std::make_shared<AsyncAccumulatingCalculator>());
  auto [near, far] = opal::http::InMemoryWebSocketPair::Create();
  sessions_.push_back(near);
  sessions_.push_back(far);
  Serve("/", far);

  ASSERT_TRUE(
      near->Send(RawText(R"({"jsonrpc":"2.0","method":"Add","params":{"a":1,"b":2},"id":1})"))
          .ok());
  auto refusal = near->Receive();
  ASSERT_TRUE(refusal.ok() && refusal->has_value());
  EXPECT_EQ((**refusal).payload.ToString(),
            R"({"error":{"code":-32601,"data":{"__type":"UnknownOperationException"},)"
            R"("message":"unknown method: Add"},"id":1,"jsonrpc":"2.0"})");
  auto end = near->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

}  // namespace
}  // namespace example::calculator
