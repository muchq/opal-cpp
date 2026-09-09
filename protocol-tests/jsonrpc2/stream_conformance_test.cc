// The authored stream conformance suite for smithy.cpp.protocols#jsonRpc2
// (ADR-0023) — normative for the stream wire the way the model's trait
// cases are for the unary wire (the smithy test traits are
// request/response-shaped and cannot express a stream). Every server-side
// case speaks RAW text frames on an in-memory pair end against the
// generated JsonRpc2ProtocolServer, so the pinned bytes are the wire; the
// client-side cases point the generated client's dialer at the same pair
// and pin what it emits and how it classifies what returns. Byte equality
// is wire equality: the runtime's JSON output is deterministic (sorted
// keys, compact).
//
// Coverage, per issue #123's checklist: the opening call (initial-request
// members in params), events both directions with the opening id echoed
// inside params, the terminal result, the terminal modeled error
// (@httpError code + fully qualified __type), each reserved-code failure
// (-32700, -32600 in every envelope shape, -32601, -32602), and the clean
// close after every terminal.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "opal/protocoltests/jsonrpc2/client.h"
#include "opal/protocoltests/jsonrpc2/server.h"
#include "smithy/client/config.h"
#include "smithy/core/error.h"
#include "smithy/core/outcome.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace opal::protocoltests::jsonrpc2 {
namespace {

// One raw wire frame: a headerless message whose payload is the envelope
// text — what a browser's send() puts on a real raw-text wire.
eventstream::Message RawText(std::string text) {
  eventstream::Message message;
  message.payload = Blob::FromString(std::move(text));
  return message;
}

// The reference echo handler: echoes prefix+text per note; "done" ends the
// session cleanly (the terminal result); "abort" ends it with the modeled
// StreamAbort (the terminal error). Async — the conformance sessions run
// the session seam, whose inline pair completions keep every case
// single-threaded and deterministic; one blocking-seam case pins parity.
class AsyncEchoHandler final : public JsonRpc2ProtocolAsyncHandler {
 public:
  opal::Outcome<EchoPayloadOutput> EchoPayload(const EchoPayloadInput&,
                                               const opal::server::RequestContext&) override {
    return EchoPayloadOutput{};
  }
  opal::Outcome<NoArgsOutput> NoArgs(const NoArgsInput&,
                                     const opal::server::RequestContext&) override {
    return NoArgsOutput{};
  }
  opal::Outcome<PutConstrainedOutput> PutConstrained(const PutConstrainedInput&,
                                                     const opal::server::RequestContext&) override {
    return PutConstrainedOutput{};
  }

  opal::eventstream::StreamTask EchoStream(EchoStreamInput input,
                                           EchoStreamAsyncServerStream& stream) override {
    const std::string prefix = input.prefix.value_or("");
    while (true) {
      auto note = co_await stream.Receive();
      if (!note.ok() || !note->has_value()) co_return opal::Unit{};
      const std::string& text = (**note).as_note().text;
      if (text == "done") co_return opal::Unit{};
      if (text == "abort") {
        opal::Error error = opal::Error::Modeled("StreamAbort", "aborted by note");
        error.set_detail(StreamAbort{.message = "aborted by note"});
        co_return error;
      }
      auto sent = co_await stream.Send(DownEvents::FromEcho(EchoedNote{.text = prefix + text}));
      if (!sent.ok()) co_return opal::Unit{};
    }
  }
};

class BlockingEchoHandler final : public JsonRpc2ProtocolHandler {
 public:
  opal::Outcome<EchoPayloadOutput> EchoPayload(const EchoPayloadInput&,
                                               const opal::server::RequestContext&) override {
    return EchoPayloadOutput{};
  }
  opal::Outcome<NoArgsOutput> NoArgs(const NoArgsInput&,
                                     const opal::server::RequestContext&) override {
    return NoArgsOutput{};
  }
  opal::Outcome<PutConstrainedOutput> PutConstrained(const PutConstrainedInput&,
                                                     const opal::server::RequestContext&) override {
    return PutConstrainedOutput{};
  }

  opal::Outcome<opal::Unit> EchoStream(const EchoStreamInput& input, EchoStreamServerStream& stream,
                                       const opal::server::RequestContext&) override {
    const std::string prefix = input.prefix.value_or("");
    while (true) {
      auto note = stream.Receive();
      if (!note.ok() || !note->has_value()) return opal::Unit{};
      const std::string& text = (**note).as_note().text;
      if (text == "done") return opal::Unit{};
      if (text == "abort") {
        opal::Error error = opal::Error::Modeled("StreamAbort", "aborted by note");
        error.set_detail(StreamAbort{.message = "aborted by note"});
        return error;
      }
      if (!stream.Send(DownEvents::FromEcho(EchoedNote{.text = prefix + text})).ok()) {
        return opal::Unit{};
      }
    }
  }
};

// One conformance session on the session seam: a fresh pair, the far end
// served by the generated server's shared-endpoint route, the near end
// returned for the case to speak raw envelopes on.
class StreamConformanceTest : public testing::Test {
 protected:
  std::shared_ptr<http::WebSocket> OpenSession() {
    server_ = std::make_unique<JsonRpc2ProtocolServer>(std::make_shared<AsyncEchoHandler>());
    auto [near, far] = http::InMemoryWebSocketPair::Create();
    sessions_.push_back(near);
    sessions_.push_back(far);
    http::HttpRequest upgrade;
    upgrade.method = "GET";
    upgrade.target = "/";
    server_->StreamRouter()->ServeSession()(upgrade, far);
    return near;
  }

  // Sends one opening envelope and returns the terminal answer's text; the
  // reserved-code bank asserts on it and on the close that follows.
  std::string TerminalFor(const std::string& opening) {
    auto peer = OpenSession();
    EXPECT_TRUE(peer->Send(RawText(opening)).ok());
    auto answer = peer->Receive();
    EXPECT_TRUE(answer.ok() && answer->has_value());
    if (!answer.ok() || !answer->has_value()) return "";
    std::string text = (**answer).payload.ToString();
    auto end = peer->Receive();
    EXPECT_TRUE(end.ok() && !end->has_value()) << "the refusal must be terminal";
    return text;
  }

  void TearDown() override {
    for (auto& session : sessions_) session->Close();
  }

  std::unique_ptr<JsonRpc2ProtocolServer> server_;
  std::vector<std::shared_ptr<http::WebSocket>> sessions_;
};

TEST_F(StreamConformanceTest, TheOpeningCallStreamsEventsWithTheIdEchoedInParams) {
  auto peer = OpenSession();
  ASSERT_TRUE(
      peer->Send(RawText(
                     R"({"jsonrpc":"2.0","method":"EchoStream","params":{"prefix":"p:"},"id":42})"))
          .ok());
  ASSERT_TRUE(
      peer
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"note","params":{"id":42,"payload":{"text":"hi"}}})"))
          .ok());
  auto echo = peer->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_TRUE((**echo).headers.empty());
  EXPECT_EQ((**echo).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"echo","params":{"id":42,"payload":{"text":"p:hi"}}})");
}

TEST_F(StreamConformanceTest, TheTerminalResultEndsTheStreamThenTheClose) {
  auto peer = OpenSession();
  ASSERT_TRUE(
      peer->Send(RawText(R"({"jsonrpc":"2.0","method":"EchoStream","params":{},"id":42})")).ok());
  ASSERT_TRUE(
      peer
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"note","params":{"id":42,"payload":{"text":"done"}}})"))
          .ok());
  auto terminal = peer->Receive();
  ASSERT_TRUE(terminal.ok() && terminal->has_value());
  EXPECT_EQ((**terminal).payload.ToString(), R"({"id":42,"jsonrpc":"2.0","result":{}})");
  auto end = peer->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());  // a vanilla peer saw one call, one response
}

TEST_F(StreamConformanceTest, TheModeledErrorIsTheTerminalErrorEnvelope) {
  // The unary error-object convention unchanged: the @httpError status as
  // the code, the fully qualified shape id in data.__type, the serialized
  // detail as data.
  auto peer = OpenSession();
  ASSERT_TRUE(
      peer->Send(RawText(R"({"jsonrpc":"2.0","method":"EchoStream","params":{},"id":42})")).ok());
  ASSERT_TRUE(
      peer
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"note","params":{"id":42,"payload":{"text":"abort"}}})"))
          .ok());
  auto terminal = peer->Receive();
  ASSERT_TRUE(terminal.ok() && terminal->has_value());
  EXPECT_EQ(
      (**terminal).payload.ToString(),
      R"({"error":{"code":409,"data":{"__type":"smithy.cpp.protocoltests.jsonrpc2#StreamAbort",)"
      R"("message":"aborted by note"},"message":"aborted by note"},"id":42,"jsonrpc":"2.0"})");
  auto end = peer->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

TEST_F(StreamConformanceTest, Minus32700ForAnUnparseableOpening) {
  EXPECT_EQ(TerminalFor("{"),
            R"({"error":{"code":-32700,"data":{"__type":"SerializationException"},)"
            R"("message":"request body is not valid JSON"},"id":null,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32600ForANonObjectEnvelope) {
  EXPECT_EQ(TerminalFor("[1,2,3]"),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"request is not a JSON-RPC 2.0 call"},"id":null,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32600ForAWrongVersion) {
  EXPECT_EQ(TerminalFor(R"({"jsonrpc":"1.0","method":"EchoStream","id":7})"),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"expected jsonrpc: \"2.0\""},"id":7,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32600ForAMissingMethod) {
  EXPECT_EQ(TerminalFor(R"({"jsonrpc":"2.0","id":3})"),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"expected a string method member"},"id":3,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32600ForANotificationOpening) {
  // Unlike the unary endpoint, a call without an id is refused: nothing
  // could answer it and nothing could echo it (ADR-0023).
  EXPECT_EQ(TerminalFor(R"({"jsonrpc":"2.0","method":"EchoStream","params":{}})"),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"the opening call must carry an id"},"id":null,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32601ForAnUnknownMethod) {
  EXPECT_EQ(TerminalFor(R"({"jsonrpc":"2.0","method":"DoesNotExist","id":4})"),
            R"({"error":{"code":-32601,"data":{"__type":"UnknownOperationException"},)"
            R"("message":"unknown method: DoesNotExist"},"id":4,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32601ForAUnaryMethodOnTheStreamEndpoint) {
  // The stream endpoint dispatches streaming operations only.
  EXPECT_EQ(TerminalFor(R"({"jsonrpc":"2.0","method":"EchoPayload","id":4})"),
            R"({"error":{"code":-32601,"data":{"__type":"UnknownOperationException"},)"
            R"("message":"unknown method: EchoPayload"},"id":4,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, Minus32602ForUndeserializableParams) {
  EXPECT_EQ(TerminalFor(R"({"jsonrpc":"2.0","method":"EchoStream","params":{"prefix":5},"id":9})"),
            R"({"error":{"code":-32602,"data":{"__type":"SerializationException"},)"
            R"("message":"EchoStreamInput.prefix: unexpected type on the wire"},)"
            R"("id":9,"jsonrpc":"2.0"})");
}

TEST_F(StreamConformanceTest, AHeaderedOpeningMessageIsRefusedWithMinus32600) {
  // Only the pair can even deliver this (Beast raw-text yields headerless
  // messages by construction): a peer speaking the eventstream envelope
  // wire into the JSON-RPC endpoint.
  auto peer = OpenSession();
  EXPECT_TRUE(peer->Send(opal::eventstream::MakeEventMessage("note", "application/json",
                                                             Blob::FromString("{}")))
                  .ok());
  auto answer = peer->Receive();
  ASSERT_TRUE(answer.ok() && answer->has_value());
  EXPECT_EQ((**answer).payload.ToString(),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"the opening message must be one JSON-RPC request envelope"},)"
            R"("id":null,"jsonrpc":"2.0"})");
  auto end = peer->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

// --- Mid-stream envelope policing (ADR-0023): after a valid opening, an
// envelope-level violation earns the reserved-code terminal error for the
// opening id, then the close — never a success terminal, never a bare
// close. The wrapper polices; the handler only ever sees a dead session.

class MidStreamConformanceTest : public StreamConformanceTest {
 protected:
  // Opens EchoStream (id 42) and proves the session is live with one echo.
  std::shared_ptr<http::WebSocket> OpenLiveStream() {
    auto peer = OpenSession();
    EXPECT_TRUE(
        peer->Send(RawText(R"({"jsonrpc":"2.0","method":"EchoStream","params":{},"id":42})")).ok());
    EXPECT_TRUE(
        peer
            ->Send(RawText(
                R"({"jsonrpc":"2.0","method":"note","params":{"id":42,"payload":{"text":"x"}}})"))
            .ok());
    auto echo = peer->Receive();
    EXPECT_TRUE(echo.ok() && echo->has_value());
    return peer;
  }

  // The violating frame's whole aftermath: exactly one terminal — the
  // expected reserved-code envelope — then the close, and in particular
  // NOT the driver's own result terminal behind it.
  void ExpectViolationTerminal(const std::shared_ptr<http::WebSocket>& peer,
                               const std::string& expected) {
    auto answer = peer->Receive();
    ASSERT_TRUE(answer.ok() && answer->has_value());
    EXPECT_EQ((**answer).payload.ToString(), expected);
    auto end = peer->Receive();
    ASSERT_TRUE(end.ok());
    EXPECT_FALSE(end->has_value()) << "a second envelope followed the violation terminal";
  }
};

TEST_F(MidStreamConformanceTest, UnparseableTextMidStreamIsMinus32700ThenTheClose) {
  auto peer = OpenLiveStream();
  ASSERT_TRUE(peer->Send(RawText("not json")).ok());
  ExpectViolationTerminal(peer,
                          R"({"error":{"code":-32700,"data":{"__type":"SerializationException"},)"
                          R"("message":"text frame is not JSON"},"id":42,"jsonrpc":"2.0"})");
}

TEST_F(MidStreamConformanceTest, ARequestEnvelopeMidStreamIsMinus32600ThenTheClose) {
  auto peer = OpenLiveStream();
  ASSERT_TRUE(
      peer->Send(RawText(R"({"jsonrpc":"2.0","method":"EchoStream","params":{},"id":43})")).ok());
  ExpectViolationTerminal(
      peer, R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"a request envelope after the opening call"},"id":42,"jsonrpc":"2.0"})");
}

TEST_F(MidStreamConformanceTest, AClientSentResultEnvelopeIsMinus32600ThenTheClose) {
  // Terminal response envelopes are server-minted; a client-sent result
  // must not read as the peer's clean close (it would skip the handler's
  // own exit paths and earn a success terminal for a violation).
  auto peer = OpenLiveStream();
  ASSERT_TRUE(peer->Send(RawText(R"({"jsonrpc":"2.0","result":{},"id":42})")).ok());
  ExpectViolationTerminal(
      peer, R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"a response envelope from the client"},"id":42,"jsonrpc":"2.0"})");
}

TEST_F(MidStreamConformanceTest, AForeignIdEchoMidStreamIsMinus32600ThenTheClose) {
  auto peer = OpenLiveStream();
  ASSERT_TRUE(
      peer
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"note","params":{"id":9,"payload":{"text":"x"}}})"))
          .ok());
  ExpectViolationTerminal(
      peer, R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"x("message":"notification for a different call's id (one stream per socket)"},)x"
            "\"id\":42,\"jsonrpc\":\"2.0\"}");
}

TEST_F(MidStreamConformanceTest, AnUnknownEventMemberFollowsTheModeledLayerRule) {
  // A grammatically valid notification naming no modeled member is a
  // MODELED-layer violation: the generated decoder rejects it and ADR-0016's
  // terminal rule applies — the session closes; unlike the envelope-level
  // cases above, no terminal envelope is promised (the binary wires behave
  // identically). Pinned so the distinction cannot drift silently.
  auto peer = OpenLiveStream();
  ASSERT_TRUE(
      peer->Send(RawText(R"({"jsonrpc":"2.0","method":"nope","params":{"id":42,"payload":{}}})"))
          .ok());
  auto end = peer->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value()) << "no envelope is promised on the modeled-layer path, only the "
                                    "close: "
                                 << (**end).payload.ToString();
}

TEST_F(StreamConformanceTest, TheBlockingSeamSpeaksTheIdenticalWire) {
  // Seam parity: the same opening, echo, and terminal bytes through the
  // blocking route (served on a thread — Receive blocks there).
  server_ = std::make_unique<JsonRpc2ProtocolServer>(std::make_shared<BlockingEchoHandler>());
  auto [near, far] = http::InMemoryWebSocketPair::Create();
  sessions_.push_back(near);
  sessions_.push_back(far);
  http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/";
  // Scope-exit join, closing the near end first so the serve thread's
  // blocked Receive drains: an ASSERT's early return must neither
  // terminate on a joinable thread nor deadlock the join.
  struct ServeJoin {
    std::shared_ptr<http::WebSocket> peer;
    std::thread thread;
    ~ServeJoin() {
      peer->Close();
      if (thread.joinable()) thread.join();
    }
  } serve{near, std::thread([serve_fn = server_->StreamRouter()->Serve(), upgrade, session = far] {
            serve_fn(upgrade, *session);
          })};

  ASSERT_TRUE(
      near->Send(RawText(
                     R"({"jsonrpc":"2.0","method":"EchoStream","params":{"prefix":"p:"},"id":42})"))
          .ok());
  ASSERT_TRUE(
      near
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"note","params":{"id":42,"payload":{"text":"hi"}}})"))
          .ok());
  auto echo = near->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"echo","params":{"id":42,"payload":{"text":"p:hi"}}})");
  ASSERT_TRUE(
      near
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"note","params":{"id":42,"payload":{"text":"done"}}})"))
          .ok());
  auto terminal = near->Receive();
  ASSERT_TRUE(terminal.ok() && terminal->has_value());
  EXPECT_EQ((**terminal).payload.ToString(), R"({"id":42,"jsonrpc":"2.0","result":{}})");
  auto end = near->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());  // the driver's close follows its terminal
}

// --- The client side of the same wire ---------------------------------

class ClientStreamConformanceTest : public testing::Test {
 protected:
  // A generated client whose dialer yields one pair end; the far end stays
  // raw — the test IS the server.
  JsonRpc2ProtocolClient MakeClient() {
    opal::ClientConfig config;
    config.retry.max_attempts = 1;
    // Never dialed: the injected websocket_dialer intercepts the stream,
    // and no unary call is made — Create just requires a transport story.
    config.endpoint = "http://127.0.0.1:1";
    config.websocket_dialer = [this](const http::WebSocketDialRequest& request)
        -> opal::Outcome<std::shared_ptr<http::WebSocket>> {
      EXPECT_EQ(request.target, "/");
      EXPECT_TRUE(request.raw_text_frames);
      auto [near, far] = http::InMemoryWebSocketPair::Create();
      peer_ = far;
      sessions_.push_back(near);
      sessions_.push_back(far);
      return near;
    };
    return JsonRpc2ProtocolClient::Create(std::move(config))
        .value_or_die("creating the conformance client");
  }

  void TearDown() override {
    for (auto& session : sessions_) session->Close();
  }

  std::shared_ptr<http::WebSocket> peer_;
  std::vector<std::shared_ptr<http::WebSocket>> sessions_;
};

TEST_F(ClientStreamConformanceTest, TheClientOpensWithThePinnedEnvelopeAndEchoesTheId) {
  auto client = MakeClient();
  EchoStreamInput input;
  input.prefix = "p:";
  auto stream = client.EchoStream(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  auto opening = peer_->Receive();
  ASSERT_TRUE(opening.ok() && opening->has_value());
  EXPECT_TRUE((**opening).headers.empty());
  EXPECT_EQ((**opening).payload.ToString(),
            R"({"id":1,"jsonrpc":"2.0","method":"EchoStream","params":{"prefix":"p:"}})");

  ASSERT_TRUE(stream->Send(UpEvents::FromNote(Note{.text = "hi"})).ok());
  auto note = peer_->Receive();
  ASSERT_TRUE(note.ok() && note->has_value());
  EXPECT_EQ((**note).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"note","params":{"id":1,"payload":{"text":"hi"}}})");

  ASSERT_TRUE(
      peer_
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"echo","params":{"id":1,"payload":{"text":"p:hi"}}})"))
          .ok());
  auto echo = stream->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  ASSERT_TRUE((**echo).is_echo());
  EXPECT_EQ((**echo).as_echo().text, "p:hi");
}

TEST_F(ClientStreamConformanceTest, TheTerminalResultIsTheStreamsCleanEnd) {
  auto client = MakeClient();
  auto stream = client.EchoStream({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(peer_->Receive().ok());  // drain the opening

  ASSERT_TRUE(peer_->Send(RawText(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ClientStreamConformanceTest, TheTerminalErrorArrivesTypedThroughTheUnaryIdentity) {
  auto client = MakeClient();
  auto stream = client.EchoStream({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(peer_->Receive().ok());  // drain the opening

  ASSERT_TRUE(
      peer_
          ->Send(RawText(R"({"jsonrpc":"2.0","error":{"code":409,"message":"aborted by note",)"
                         R"("data":{"__type":"smithy.cpp.protocoltests.jsonrpc2#StreamAbort",)"
                         R"("message":"aborted by note"}},"id":1})"))
          .ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "StreamAbort");
  const StreamAbort* detail = outcome.error().detail<StreamAbort>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->message, "aborted by note");
}

TEST_F(ClientStreamConformanceTest, AReservedCodeErrorSurfacesAsTheGenericTerminal) {
  auto client = MakeClient();
  auto stream = client.EchoStream({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(peer_->Receive().ok());  // drain the opening

  ASSERT_TRUE(
      peer_
          ->Send(RawText(
              R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"unknown method: EchoStream"},)"
              R"("id":1})"))
          .ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_NE(outcome.error().message().find("unknown method"), std::string::npos)
      << outcome.error().message();
}

}  // namespace
}  // namespace opal::protocoltests::jsonrpc2
