// The compile-the-output harness (issue #48). Most of the test is the build:
// including every generated header and linking all six gauntlet libraries
// forces the generator's output for the hostile model through the compiler on
// every platform CI runs. The assertions below then spot-check the escaping
// contract itself — keyword members, hostile enum wire values, extreme
// numeric bounds — so a silent change in the escaping scheme fails loudly
// here rather than in a consumer's build.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "compile/gauntlet/cbor/client.h"
#include "compile/gauntlet/cbor/server.h"
#include "compile/gauntlet/jsonrpc/client.h"
#include "compile/gauntlet/jsonrpc/server.h"
#include "compile/gauntlet/rest/client.h"
#include "compile/gauntlet/rest/server.h"
#include "compile/streaming/cbor/server.h"
#include "compile/streaming/rest/server.h"

namespace {

namespace rest = compile::gauntlet::rest;

TEST(GauntletCompileTest, KeywordMembersGetTrailingUnderscores) {
  rest::RunGauntletInput input;
  input.name = "escape me";
  input.class_ = "keyword";
  input.namespace_ = "keyword";
  input.template_ = "keyword";
  input.operator_ = true;
  input.delete_ = false;
  input.int_ = 7;
  input.double_ = 1.5;
  input.union_ = "keyword";
  input.default_ = "keyword";
  input.friend_ = "keyword";
  input.this_ = "keyword";
  input.auto_ = "keyword";
  input.register_ = 9;
  input.value = "not a keyword";
  input.kind = "not a keyword";
  input._leadingUnderscore = "kept verbatim";
  EXPECT_EQ(input.int_, 7);
  EXPECT_EQ(input, input);

  rest::GetReportInput report;
  report.class_ = "label";
  report.switch_ = "query";
  report.case_ = "header";
  EXPECT_EQ(report.class_, "label");

  rest::GauntletRejected rejected;
  rejected.message = "still compiles";
  rejected.class_ = "keyword";
  EXPECT_EQ(rejected.class_, "keyword");
}

TEST(GauntletCompileTest, HostileEnumValuesRoundTrip) {
  using Enum = rest::HostileEnum;
  const struct {
    Enum::Value value;
    const char* wire;
  } cases[] = {
      {Enum::Value::kQuote, "he said \"more\""},     {Enum::Value::kBackslash, "C:\\temp\\new"},
      {Enum::Value::kNewline, "line one\nline two"}, {Enum::Value::kTrickyRaw, ")__smithy\""},
      {Enum::Value::kUnicodeValue, "caf\xc3\xa9"},
  };
  for (const auto& c : cases) {
    const Enum parsed = Enum::FromString(c.wire);
    EXPECT_EQ(parsed.value(), c.value) << c.wire;
    EXPECT_EQ(parsed.ToString(), c.wire);
  }
  const Enum unknown = Enum::FromString("never modeled");
  EXPECT_EQ(unknown.value(), Enum::Value::kUnknown);
  EXPECT_EQ(unknown.ToString(), "never modeled");
}

TEST(GauntletCompileTest, IntEnumCoversInt32Extremes) {
  EXPECT_EQ(static_cast<std::int32_t>(rest::HostileIntEnum::kBottom),
            std::numeric_limits<std::int32_t>::min());
  EXPECT_EQ(static_cast<std::int32_t>(rest::HostileIntEnum::kTop),
            std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(static_cast<std::int32_t>(rest::HostileIntEnum::kNothing), 0);
}

TEST(GauntletCompileTest, UnionKeywordVariantsWork) {
  const auto number = rest::HostileUnion::FromInt(42);
  ASSERT_TRUE(number.is_int_());
  EXPECT_EQ(number.as_int_(), 42);

  const auto text = rest::HostileUnion::FromClass("keyword variant");
  ASSERT_TRUE(text.is_class_());
  EXPECT_EQ(text.as_class_(), "keyword variant");
  EXPECT_FALSE(text.is_int_());

  rest::Node leaf;
  leaf.label = "leaf";
  const auto branch = rest::HostileUnion::FromNode(leaf);
  ASSERT_TRUE(branch.is_node());
  EXPECT_EQ(branch.as_node().label, "leaf");
}

TEST(GauntletCompileTest, RecursiveShapesUseValueSemanticBoxes) {
  rest::Node root;
  root.label = "root";
  rest::Node child;
  child.label = "child";
  root.next = opal::Boxed<rest::Node>(child);
  root.children = std::vector<rest::Node>{child};
  const rest::Node copy = root;  // deep copy through the box
  EXPECT_EQ(copy, root);
  EXPECT_EQ((*copy.next)->label, "child");
}

// The other two protocols generate the same shapes into their own
// namespaces; touching one type from each keeps all their headers in the
// build even if the includes above ever change.
TEST(GauntletCompileTest, EveryProtocolEmitsTheGauntletShapes) {
  compile::gauntlet::cbor::RunGauntletInput cbor_input;
  cbor_input.class_ = "cbor";
  EXPECT_EQ(cbor_input.class_, "cbor");

  compile::gauntlet::jsonrpc::RunGauntletInput jsonrpc_input;
  jsonrpc_input.class_ = "jsonrpc";
  EXPECT_EQ(jsonrpc_input.class_, "jsonrpc");
}

// The streaming server halves (ADR-0016): the handler's streaming signature
// and the StreamRouter wiring must compile against the transport-neutral
// runtime alone — no Boost in this target. The client halves link the Beast
// dialer and compile in streaming_compile_test instead.
TEST(GauntletCompileTest, StreamingServersExposeAStreamRouter) {
  namespace rest_stream = compile::streaming::rest;
  struct Handler : rest_stream::RelayHandler {
    opal::Outcome<opal::Unit> Converse(
        const rest_stream::ConverseInput& input,
        opal::eventstream::EventStream<rest_stream::ServerEvents, rest_stream::ClientEvents>&
            stream,
        const opal::server::RequestContext&) override {
      rest_stream::MemberJoined joined;
      joined.member = input.room;
      (void)stream.Send(rest_stream::ServerEvents::FromJoined(joined));
      return opal::Unit{};
    }
    opal::Outcome<opal::Unit> Watch(
        const rest_stream::WatchInput&,
        opal::eventstream::EventStream<rest_stream::ServerEvents, opal::eventstream::NoEvents>&
            stream,
        const opal::server::RequestContext&) override {
      stream.Close();
      return opal::Unit{};
    }
  };
  rest_stream::RelayServer server(std::make_shared<Handler>());
  EXPECT_NE(server.StreamRouter(), nullptr);
  EXPECT_NE(server.Handler(), nullptr);

  // The rpcv2Cbor face generates the same event unions into its namespace.
  const auto event = compile::streaming::cbor::ServerEvents::FromMessage([] {
    compile::streaming::cbor::ChatMessage message;
    message.text = "compiles";
    return message;
  }());
  EXPECT_TRUE(event.is_message());
}

}  // namespace
