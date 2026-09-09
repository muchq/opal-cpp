// Pins ADR-0016's envelope convention: the exact header layout the Make
// helpers mint (via Message equality — the frame codec's byte-exactness
// tests carry it the rest of the way to bytes), round-trips for both kinds,
// the reserved initial-response name, and the hostile bank — every
// malformed envelope shape must surface as Error::Serialization, never as a
// wrong envelope.

#include "smithy/eventstream/envelope.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "smithy/eventstream/frame.h"

namespace opal::eventstream {
namespace {

EventEnvelope ParseOrDie(const Message& message) {
  auto envelope = ParseEnvelope(message);
  EXPECT_TRUE(envelope.ok()) << envelope.error().message();
  return envelope.ok() ? std::move(*envelope) : EventEnvelope{};
}

void ExpectSerializationError(const Message& message, const char* why) {
  const auto envelope = ParseEnvelope(message);
  ASSERT_FALSE(envelope.ok()) << why;
  EXPECT_EQ(envelope.error().kind(), ErrorKind::kSerialization) << why;
}

TEST(EnvelopeTest, EventMessagesCarryThePinnedHeaderLayout) {
  // The convention is the wire contract: header names, values, and order
  // are pinned via Message equality (EncodeMessage is deterministic over
  // this, so equal messages are equal bytes).
  const Message expected{.headers = {{":message-type", "event"},
                                     {":event-type", "chat"},
                                     {":content-type", "application/json"}},
                         .payload = Blob::FromString("{\"text\":\"hi\"}")};
  EXPECT_EQ(MakeEventMessage("chat", "application/json", Blob::FromString("{\"text\":\"hi\"}")),
            expected);
}

TEST(EnvelopeTest, ExceptionMessagesCarryThePinnedHeaderLayout) {
  const Message expected{.headers = {{":message-type", "exception"},
                                     {":exception-type", "RoomFull"},
                                     {":content-type", "application/cbor"}},
                         .payload = Blob::FromString("cbor bytes")};
  EXPECT_EQ(MakeExceptionMessage("RoomFull", "application/cbor", Blob::FromString("cbor bytes")),
            expected);
}

TEST(EnvelopeTest, AnEmptyContentTypeOmitsTheHeader) {
  // The encode-side mirror of parse tolerance: no ":content-type" header
  // at all, not one with an empty value.
  const Message event = MakeEventMessage("chat", "", Blob());
  EXPECT_EQ(event.FindHeader(":content-type"), nullptr);
  const Message exception = MakeExceptionMessage("Boom", "", Blob());
  EXPECT_EQ(exception.FindHeader(":content-type"), nullptr);
}

TEST(EnvelopeTest, EventEnvelopesRoundTrip) {
  const EventEnvelope envelope =
      ParseOrDie(MakeEventMessage("message", "application/json", Blob::FromString("payload")));
  EXPECT_EQ(envelope.kind, EventEnvelope::Kind::kEvent);
  EXPECT_EQ(envelope.type, "message");
  EXPECT_EQ(envelope.content_type, "application/json");
  EXPECT_EQ(envelope.payload, Blob::FromString("payload"));
}

TEST(EnvelopeTest, ExceptionEnvelopesRoundTrip) {
  const EventEnvelope envelope = ParseOrDie(
      MakeExceptionMessage("RoomFull", "application/cbor", Blob::FromString("error payload")));
  EXPECT_EQ(envelope.kind, EventEnvelope::Kind::kException);
  EXPECT_EQ(envelope.type, "RoomFull");
  EXPECT_EQ(envelope.content_type, "application/cbor");
  EXPECT_EQ(envelope.payload, Blob::FromString("error payload"));
}

TEST(EnvelopeTest, EnvelopesSurviveTheFramingCodec) {
  // The full wire path a streaming session takes: mint, frame, unframe,
  // parse.
  const std::string frame =
      EncodeMessage(MakeEventMessage("chat", "application/json", Blob::FromString("hello")))
          .value_or_die("encoding an event message");
  const auto decoded = DecodeMessage(frame);
  ASSERT_TRUE(decoded.ok() && decoded->has_value());
  const EventEnvelope envelope = ParseOrDie((*decoded)->message);
  EXPECT_EQ(envelope.kind, EventEnvelope::Kind::kEvent);
  EXPECT_EQ(envelope.type, "chat");
  EXPECT_EQ(envelope.content_type, "application/json");
  EXPECT_EQ(envelope.payload, Blob::FromString("hello"));
}

TEST(EnvelopeTest, AMissingContentTypeParsesAsEmpty) {
  const EventEnvelope envelope =
      ParseOrDie(Message{.headers = {{":message-type", "event"}, {":event-type", "chat"}},
                         .payload = Blob::FromString("x")});
  EXPECT_EQ(envelope.kind, EventEnvelope::Kind::kEvent);
  EXPECT_EQ(envelope.type, "chat");
  EXPECT_EQ(envelope.content_type, "");
}

TEST(EnvelopeTest, TheInitialResponseNameIsReservedOutsideTheMemberNameSpace) {
  // The reserved name is the contract (ADR-0016): a leading ':' keeps it
  // from ever colliding with a Smithy member name, and it travels as an
  // ordinary event.
  EXPECT_EQ(kInitialResponseEventType, ":initial-response");
  const EventEnvelope envelope = ParseOrDie(
      MakeEventMessage(kInitialResponseEventType, "application/json", Blob::FromString("{}")));
  EXPECT_EQ(envelope.kind, EventEnvelope::Kind::kEvent);
  EXPECT_EQ(envelope.type, kInitialResponseEventType);
}

TEST(EnvelopeTest, MissingOrNonStringMessageTypeIsAHardError) {
  ExpectSerializationError(Message{}, "no headers at all");
  ExpectSerializationError(Message{.headers = {{":event-type", "chat"}}},
                           "typed but no :message-type");
  ExpectSerializationError(Message{.headers = {{":message-type", std::int32_t{7}}}},
                           ":message-type with an int value");
  ExpectSerializationError(Message{.headers = {{":message-type", Blob::FromString("event")}}},
                           ":message-type with a byte-array value spelling 'event'");
}

TEST(EnvelopeTest, UnknownMessageTypesAreHardErrors) {
  // Unknown kinds must fail loudly: silently skipping a message would
  // desynchronize a typed stream. Case matters — the convention is exact.
  for (const char* unknown : {"", "Event", "EXCEPTION", "events", "error", "initial-response"}) {
    ExpectSerializationError(
        Message{.headers = {{":message-type", std::string(unknown)}, {":event-type", "chat"}}},
        unknown);
  }
}

TEST(EnvelopeTest, AnEventWithoutAStringEventTypeIsAHardError) {
  ExpectSerializationError(Message{.headers = {{":message-type", "event"}}},
                           "event without :event-type");
  ExpectSerializationError(
      Message{.headers = {{":message-type", "event"}, {":event-type", std::int32_t{1}}}},
      ":event-type with an int value");
  // The other kind's type header cannot stand in.
  ExpectSerializationError(
      Message{.headers = {{":message-type", "event"}, {":exception-type", "Boom"}}},
      "event carrying only :exception-type");
}

TEST(EnvelopeTest, AnExceptionWithoutAStringExceptionTypeIsAHardError) {
  ExpectSerializationError(Message{.headers = {{":message-type", "exception"}}},
                           "exception without :exception-type");
  ExpectSerializationError(
      Message{.headers = {{":message-type", "exception"}, {":exception-type", true}}},
      ":exception-type with a bool value");
  ExpectSerializationError(
      Message{.headers = {{":message-type", "exception"}, {":event-type", "chat"}}},
      "exception carrying only :event-type");
}

TEST(EnvelopeTest, ANonStringContentTypeIsAHardError) {
  // Absence is tolerated; a present header of the wrong wire type is not.
  ExpectSerializationError(Message{.headers = {{":message-type", "event"},
                                               {":event-type", "chat"},
                                               {":content-type", std::int64_t{42}}}},
                           ":content-type with a long value");
}

TEST(EnvelopeTest, EnvelopesRenderThroughTheDebugPrinter) {
  // Exact output is deliberately unpinned — debug text is not a format.
  const EventEnvelope envelope =
      ParseOrDie(MakeExceptionMessage("Boom", "application/json", Blob::FromString("x")));
  const std::string rendered = DebugString(envelope);
  EXPECT_NE(rendered.find("exception"), std::string::npos);
  EXPECT_NE(rendered.find("\"Boom\""), std::string::npos);
}

}  // namespace
}  // namespace opal::eventstream
