// Out-of-tree acceptance for the event-stream framing codec (ADR-0014
// slice 1): the consumer surface exactly as the frame.h contract teaches
// it — plain-value headers, the canonical chunked decode loop, FindString
// dispatch, and the core-Timestamp interop — consumed through the module
// boundary like any other @smithy_cpp runtime target.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "opal/core/blob.h"
#include "opal/core/print.h"
#include "opal/core/timestamp.h"
#include "opal/eventstream/frame.h"

namespace {

using opal::Blob;
using opal::Timestamp;
using opal::eventstream::DecodeMessage;
using opal::eventstream::EncodeMessage;
using opal::eventstream::Message;

TEST(EventStreamConsumerTest, TheDocumentedTransportLoopDrainsAChunkedStream) {
  // Two frames arriving from a "socket" seven bytes at a time; the inner
  // loop is the one documented on DecodeMessage, verbatim.
  const Message first{.headers = {{":event-type", "chat"},
                                  {"seq", 1},
                                  {"at", Timestamp::FromEpochMilliseconds(1721400000000)}},
                      .payload = Blob::FromString("hello")};
  const Message second{.headers = {{":event-type", "end"}}};
  const auto first_frame = EncodeMessage(first);
  const auto second_frame = EncodeMessage(second);
  ASSERT_TRUE(first_frame.ok() && second_frame.ok());
  const std::string wire = *first_frame + *second_frame;

  std::vector<Message> received;
  std::string buffer;
  std::size_t offset = 0;  // the "socket" read position
  while (received.size() < 2) {
    ASSERT_LT(offset, wire.size()) << "stream drained before both messages arrived";
    buffer += wire.substr(offset, 7);
    offset += 7;
    while (true) {
      auto decoded = DecodeMessage(buffer);
      ASSERT_TRUE(decoded.ok()) << decoded.error().message();  // stream is dead: stop feeding
      if (!decoded->has_value()) break;                        // read more bytes, come back
      received.push_back((**decoded).message);
      buffer.erase(0, (**decoded).bytes_consumed);
    }
  }
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[0], first);
  EXPECT_EQ(received[1], second);

  // Dispatch the way streaming consumers will: FindString on the routing key.
  ASSERT_NE(received[0].FindString(":event-type"), nullptr);
  EXPECT_EQ(*received[0].FindString(":event-type"), "chat");
  ASSERT_NE(received[1].FindString(":event-type"), nullptr);
  EXPECT_EQ(*received[1].FindString(":event-type"), "end");
  EXPECT_EQ(received[1].FindString("seq"), nullptr);  // absent on the end event

  // The timestamp header comes back as the runtime's own opal::Timestamp.
  const auto* at = received[0].FindHeader("at");
  ASSERT_NE(at, nullptr);
  EXPECT_EQ(std::get<Timestamp>(*at).epoch_milliseconds(), 1721400000000);
}

TEST(EventStreamConsumerTest, CorruptionSurfacesAsAnErrorAConsumerCanLog) {
  const auto frame = EncodeMessage(Message{.headers = {{":event-type", "chat"}}});
  ASSERT_TRUE(frame.ok());
  std::string corrupted = *frame;
  corrupted[corrupted.size() / 2] = static_cast<char>(corrupted[corrupted.size() / 2] ^ 0x01);
  const auto decoded = DecodeMessage(corrupted);
  ASSERT_FALSE(decoded.ok());
  EXPECT_NE(decoded.error().message().find("eventstream"), std::string::npos);
  // And the message itself debug-renders for that log line.
  const std::string rendered = opal::DebugString(Message{.headers = {{":event-type", "chat"}}});
  EXPECT_NE(rendered.find(":event-type"), std::string::npos);
}

}  // namespace
