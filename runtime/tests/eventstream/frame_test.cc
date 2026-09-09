// Pins ADR-0014's framing codec: byte-exact vectors against an independent
// CRC32 implementation (python zlib produced the expected bytes), a
// round-trip across every header wire type, the incremental feed-me-more
// contract, and the hostile bank — every corrupted byte must surface as an
// error, never as a wrong message.

#include "smithy/eventstream/frame.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace opal::eventstream {
namespace {

std::string EncodeOrDie(const Message& message) {
  auto frame = EncodeMessage(message);
  EXPECT_TRUE(frame.ok()) << frame.error().message();
  return frame.ok() ? *frame : std::string();
}

DecodedFrame DecodeOrDie(std::string_view buffer) {
  auto decoded = DecodeMessage(buffer);
  EXPECT_TRUE(decoded.ok()) << decoded.error().message();
  EXPECT_TRUE(decoded.ok() && decoded->has_value());
  return decoded.ok() && decoded->has_value() ? std::move(**decoded) : DecodedFrame{};
}

// Test-local CRC32 (zlib polynomial) for crafting hostile frames whose
// preludes must still pass the CRC gate; deliberately independent of the
// codec's internal implementation.
std::uint32_t TestCrc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char byte : bytes) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void AppendU32(std::string& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

// A frame with the given declared lengths, both CRCs valid, sized to the
// declared total — for reaching the bounds checks behind the CRC gate.
std::string CraftFrame(std::uint32_t total, std::uint32_t headers_length) {
  std::string frame;
  AppendU32(frame, total);
  AppendU32(frame, headers_length);
  AppendU32(frame, TestCrc32(frame));
  if (total >= 16) {
    frame.resize(total - 4, '\0');
    AppendU32(frame, TestCrc32(frame));
  }
  return frame;
}

std::string BytesToString(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// A frame wrapping `block` as its header block (no payload), both CRCs
// valid — so only the header parser can reject it.
std::string FrameWithBlock(std::string_view block) {
  std::string frame;
  AppendU32(frame, static_cast<std::uint32_t>(16 + block.size()));
  AppendU32(frame, static_cast<std::uint32_t>(block.size()));
  AppendU32(frame, TestCrc32(frame));
  frame += block;
  AppendU32(frame, TestCrc32(frame));
  return frame;
}

// Byte-exact both ways: encode must produce exactly `expected`, and
// `expected` must decode back to exactly `message` (the decode direction
// kills decode-side tag-swap/sign-extension mutants that the encode
// comparison alone would miss).
void ExpectByteExact(const Message& message, const std::vector<std::uint8_t>& expected,
                     const char* why) {
  const auto frame = EncodeMessage(message);
  ASSERT_TRUE(frame.ok()) << why << ": " << frame.error().message();
  ASSERT_EQ(frame->size(), expected.size()) << why;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>((*frame)[i]), expected[i]) << why << " byte " << i;
  }
  const auto decoded = DecodeMessage(BytesToString(expected));
  ASSERT_TRUE(decoded.ok() && decoded->has_value()) << why;
  EXPECT_EQ((*decoded)->message, message) << why;
  EXPECT_EQ((*decoded)->bytes_consumed, expected.size()) << why;
}

TEST(EventStreamFrameTest, TheEmptyMessageIsByteExact) {
  // Independent vector: python zlib computed both CRCs.
  const std::vector<std::uint8_t> expected = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
                                              0x05, 0xC2, 0x48, 0xEB, 0x7D, 0x98, 0xC8, 0xFF};
  const std::string frame = EncodeOrDie(Message{});
  ASSERT_EQ(frame.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>(frame[i]), expected[i]) << "byte " << i;
  }
  const auto decoded = DecodeOrDie(frame);
  EXPECT_TRUE(decoded.message.headers.empty());
  EXPECT_EQ(decoded.message.payload.size(), 0u);
  EXPECT_EQ(decoded.bytes_consumed, 16u);
}

TEST(EventStreamFrameTest, AStringHeaderAndPayloadAreByteExact) {
  const std::vector<std::uint8_t> expected = {
      0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x13, 0xE8, 0xBD, 0x3E, 0xC3, 0x0B,
      0x3A, 0x65, 0x76, 0x65, 0x6E, 0x74, 0x2D, 0x74, 0x79, 0x70, 0x65, 0x07, 0x00,
      0x04, 0x63, 0x68, 0x61, 0x74, 0x68, 0x69, 0xDD, 0xF4, 0xE3, 0xAC};
  const std::string frame =
      EncodeOrDie(Message{.headers = {{":event-type", "chat"}}, .payload = Blob::FromString("hi")});
  ASSERT_EQ(frame.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>(frame[i]), expected[i]) << "byte " << i;
  }
}

TEST(EventStreamFrameTest, WireTypeVectorsAreByteExactBothWays) {
  // One independent vector per wire type (python struct + zlib produced the
  // bytes). Round-trips cannot catch a wire-tag renumbering: encoder and
  // decoder share the enum, so both sides would drift together. Only bytes
  // pinned outside the codec kill that mutant class.
  ExpectByteExact(Message{.headers = {{"flag", true}}, .payload = Blob::FromString("yes")},
                  {0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x06, 0xE1, 0xB1, 0x8F, 0xAF, 0x04,
                   0x66, 0x6C, 0x61, 0x67, 0x00, 0x79, 0x65, 0x73, 0x2A, 0x0A, 0x2C, 0x32},
                  "bool true = tag 0, no value bytes");
  ExpectByteExact(Message{.headers = {{"flag", false}}, .payload = Blob::FromString("no")},
                  {0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x06, 0xDC, 0xD1, 0xA6, 0x1F,
                   0x04, 0x66, 0x6C, 0x61, 0x67, 0x01, 0x6E, 0x6F, 0x28, 0x21, 0x53, 0x07},
                  "bool false = tag 1, no value bytes");
  ExpectByteExact(Message{.headers = {{"b", std::int8_t{-1}}}, .payload = Blob::FromString("i8")},
                  {0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x04, 0x8D, 0xEF, 0x79,
                   0x52, 0x01, 0x62, 0x02, 0xFF, 0x69, 0x38, 0xE0, 0x79, 0x2A, 0x42},
                  "byte = tag 2");
  ExpectByteExact(Message{.headers = {{"s", std::int16_t{-2}}}, .payload = Blob::FromString("i16")},
                  {0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x05, 0x45, 0xD8, 0xF7, 0xA5,
                   0x01, 0x73, 0x03, 0xFF, 0xFE, 0x69, 0x31, 0x36, 0xD7, 0xC6, 0x1B, 0x3A},
                  "short = tag 3, big-endian");
  ExpectByteExact(Message{.headers = {{"i", std::int32_t{-3}}}, .payload = Blob::FromString("i32")},
                  {0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x07, 0xD1, 0x16, 0xC5, 0xE9, 0x01,
                   0x69, 0x04, 0xFF, 0xFF, 0xFF, 0xFD, 0x69, 0x33, 0x32, 0xCC, 0xFD, 0x6E, 0x86},
                  "int = tag 4, big-endian");
  ExpectByteExact(
      Message{.headers = {{"l", std::int64_t{-4}}}, .payload = Blob::FromString("i64")},
      {0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x0B, 0x2D, 0x20, 0x2F, 0x02, 0x01, 0x6C, 0x05,
       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFC, 0x69, 0x36, 0x34, 0xC2, 0x12, 0xDB, 0x29},
      "long = tag 5, big-endian");
  ExpectByteExact(
      Message{.headers = {{"bin", Blob::FromString(std::string("\x00\xFF\x10", 3))}},
              .payload = Blob::FromString("blob")},
      {0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x0A, 0x5A, 0x27, 0x1F, 0x94, 0x03, 0x62, 0x69,
       0x6E, 0x06, 0x00, 0x03, 0x00, 0xFF, 0x10, 0x62, 0x6C, 0x6F, 0x62, 0x6D, 0x2F, 0x76, 0x70},
      "byte-array = tag 6, u16 length prefix");
  ExpectByteExact(
      Message{.headers = {{"str", std::string("\xC3\xA9", 2)}},
              .payload = Blob::FromString("utf8")},
      {0x00, 0x00, 0x00, 0x1D, 0x00, 0x00, 0x00, 0x09, 0x84, 0x8E, 0x34, 0xFE, 0x03, 0x73, 0x74,
       0x72, 0x07, 0x00, 0x02, 0xC3, 0xA9, 0x75, 0x74, 0x66, 0x38, 0x44, 0x06, 0xF5, 0x90},
      "string = tag 7, u16 length prefix");
  ExpectByteExact(Message{.headers = {{"ts", Timestamp::FromEpochMilliseconds(-1)}},
                          .payload = Blob::FromString("time")},
                  {0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x0C, 0xAD, 0x55, 0xBC,
                   0x46, 0x02, 0x74, 0x73, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                   0xFF, 0xFF, 0x74, 0x69, 0x6D, 0x65, 0x0C, 0x66, 0x50, 0xB5},
                  "timestamp = tag 8, not the long tag");
  Uuid uuid;
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
    uuid.bytes[i] = static_cast<std::uint8_t>(i);
  }
  ExpectByteExact(
      Message{.headers = {{"id", uuid}}, .payload = Blob::FromString("uuid")},
      {0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x14, 0x8E, 0x49, 0x6F, 0xD1, 0x02, 0x69,
       0x64, 0x09, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
       0x0C, 0x0D, 0x0E, 0x0F, 0x75, 0x75, 0x69, 0x64, 0xDA, 0x2E, 0xD3, 0xD8},
      "uuid = tag 9, 16 raw bytes with no length prefix");
  ExpectByteExact(Message{.headers = {{"e", std::string()}}},
                  {0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x05, 0xBD, 0x48, 0x33,
                   0x14, 0x01, 0x65, 0x07, 0x00, 0x00, 0xBB, 0x9F, 0x51, 0x2C},
                  "empty string keeps its explicit u16 zero length");
}

TEST(EventStreamFrameTest, EveryHeaderTypeRoundTrips) {
  Uuid uuid;
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
    uuid.bytes[i] = static_cast<std::uint8_t>(i * 17);
  }
  const Message message{.headers =
                            {
                                {"t", true},
                                {"f", false},
                                {"byte", std::int8_t{-7}},
                                {"short", std::int16_t{-30000}},
                                {"int", std::int32_t{-2000000000}},
                                {"long", std::int64_t{-9000000000000000000LL}},
                                {"bytes", Blob::FromString(std::string("\x00\xFF\x7F", 3))},
                                {"string", "héllo"},
                                {"when", Timestamp::FromEpochMilliseconds(1721400000000)},
                                {"id", uuid},
                            },
                        .payload = Blob::FromString("payload bytes")};
  const std::string frame = EncodeOrDie(message);
  const auto decoded = DecodeOrDie(frame);
  EXPECT_EQ(decoded.message, message);
  EXPECT_EQ(decoded.bytes_consumed, frame.size());
}

TEST(EventStreamFrameTest, LiteralsSelectTheExpectedHeaderAlternative) {
  // C++20 variant converting-construction, pinned so a future alternative
  // added to HeaderValue cannot silently re-route plain-value spellings: a
  // string literal is a string (never bool), a plain int is an int32, and
  // byte/short require their explicit fixed-width types.
  const Header text{"n", "text"};
  EXPECT_TRUE(std::holds_alternative<std::string>(text.value));
  const Header number{"n", 42};
  EXPECT_TRUE(std::holds_alternative<std::int32_t>(number.value));
  const Header truth{"n", true};
  EXPECT_TRUE(std::holds_alternative<bool>(truth.value));
  const Header wide{"n", std::int64_t{42}};
  EXPECT_TRUE(std::holds_alternative<std::int64_t>(wide.value));
}

TEST(EventStreamFrameTest, EveryStrictPrefixAsksForMoreBytes) {
  // The incremental contract: an incomplete buffer is never an error.
  const std::string frame =
      EncodeOrDie(Message{.headers = {{":event-type", "chat"}}, .payload = Blob::FromString("hi")});
  for (std::size_t length = 0; length < frame.size(); ++length) {
    const auto decoded = DecodeMessage(std::string_view(frame).substr(0, length));
    ASSERT_TRUE(decoded.ok()) << "prefix " << length << ": " << decoded.error().message();
    EXPECT_FALSE(decoded->has_value()) << "prefix " << length;
  }
  EXPECT_TRUE(DecodeMessage(frame).ok());
}

TEST(EventStreamFrameTest, EveryFlippedByteIsAnErrorNeverAWrongMessage) {
  // The CRCs' whole job: corruption surfaces as an error, not as content.
  const Message message{.headers = {{":event-type", "chat"}, {"n", std::int32_t{42}}},
                        .payload = Blob::FromString("payload")};
  const std::string frame = EncodeOrDie(message);
  for (std::size_t i = 0; i < frame.size(); ++i) {
    std::string corrupted = frame;
    corrupted[i] = static_cast<char>(corrupted[i] ^ 0x01);
    // A hard error every time: CRC32 detects all single-bit errors, so no
    // flip can even demote the buffer to "incomplete" — and a regression
    // that turned corruption into feed-me-more would stall a live stream.
    EXPECT_FALSE(DecodeMessage(corrupted).ok()) << "flipped byte " << i;
  }
}

TEST(EventStreamFrameTest, BackToBackFramesDecodeInOrder) {
  const std::string first = EncodeOrDie(Message{.payload = Blob::FromString("one")});
  const std::string second =
      EncodeOrDie(Message{.headers = {{"n", std::int8_t{2}}}, .payload = Blob::FromString("two")});
  std::string buffer = first + second;

  const auto a = DecodeOrDie(buffer);
  EXPECT_EQ(a.bytes_consumed, first.size());
  EXPECT_EQ(a.message.payload.ToString(), "one");
  const auto b = DecodeOrDie(std::string_view(buffer).substr(a.bytes_consumed));
  EXPECT_EQ(b.bytes_consumed, second.size());
  EXPECT_EQ(b.message.payload.ToString(), "two");

  // Trailing garbage changes nothing: the message CRC is read at total-4,
  // not at the buffer's end, and consumption stops at the frame boundary.
  const auto with_garbage = DecodeOrDie(first + "garbage");
  EXPECT_EQ(with_garbage.bytes_consumed, first.size());
  EXPECT_EQ(with_garbage.message.payload.ToString(), "one");
}

TEST(EventStreamFrameTest, DeclaredLengthsOutOfBoundsAreRejectedBehindTheCrcGate) {
  // CraftFrame produces valid CRCs, so these reach the bounds checks —
  // which must reject before any buffering decision trusts the lengths.
  const auto too_small = DecodeMessage(CraftFrame(15, 0));
  EXPECT_FALSE(too_small.ok());
  const auto headers_over_block_limit = DecodeMessage(CraftFrame(1024, 512 * 1024));
  EXPECT_FALSE(headers_over_block_limit.ok());
  const auto headers_over_total = DecodeMessage(CraftFrame(32, 20));
  EXPECT_FALSE(headers_over_total.ok());

  // An over-limit declared total must reject immediately from the 12-byte
  // prelude — a decoder that waited for 2^32-ish bytes would buffer forever.
  std::string huge;
  AppendU32(huge, 0x7FFFFFFFu);
  AppendU32(huge, 0);
  AppendU32(huge, TestCrc32(huge));
  const auto over_limit = DecodeMessage(huge);
  EXPECT_FALSE(over_limit.ok());

  // The u32 extreme on both fields at once — the conversion to size_t must
  // stay exact and rejected, again from the 12 prelude bytes alone
  // (CraftFrame would materialize the declared 4 GiB).
  std::string extreme;
  AppendU32(extreme, 0xFFFFFFFFu);
  AppendU32(extreme, 0xFFFFFFFFu);
  AppendU32(extreme, TestCrc32(extreme));
  EXPECT_FALSE(DecodeMessage(extreme).ok());
}

TEST(EventStreamFrameTest, ABadPreludeCrcErrorsEvenWhenTheBufferLooksIncomplete) {
  // total=100/headers=0 would be feed-me-more for a 12-byte buffer — but
  // the prelude CRC is wrong, and the CRC gates the lengths: this must be a
  // hard error, never nullopt, or corrupt lengths drive buffering
  // decisions (a transport would wait forever for bytes that never come).
  // python3: struct.pack(">II", 100, 0) + (zlib.crc32 ^ 0xFFFFFFFF).
  const std::vector<std::uint8_t> prelude = {0x00, 0x00, 0x00, 0x64, 0x00, 0x00,
                                             0x00, 0x00, 0xF6, 0x6F, 0xF1, 0x1B};
  EXPECT_FALSE(DecodeMessage(BytesToString(prelude)).ok());
}

TEST(EventStreamFrameTest, MalformedHeaderBlocksAreHardErrors) {
  // Frames whose CRCs are valid but whose header blocks are hostile — only
  // the header parser can reject them.
  // A zero-length header name.
  EXPECT_FALSE(DecodeMessage(FrameWithBlock(std::string(1, '\0'))).ok());
  // A name that runs past the block.
  EXPECT_FALSE(DecodeMessage(FrameWithBlock(std::string(1, '\x05') + "ab")).ok());
  // A name whose type byte is missing entirely (block ends after the name).
  EXPECT_FALSE(DecodeMessage(FrameWithBlock(std::string(1, '\x01') + "n")).ok());
  // A string value whose declared length runs past the block.
  EXPECT_FALSE(
      DecodeMessage(FrameWithBlock(std::string(1, '\x01') + "n" + '\x07' + '\x00' + '\x09' + "ab"))
          .ok());
}

TEST(EventStreamFrameTest, AllUnknownWireTypes10Through255AreHardErrors) {
  // Not just tag 10: the whole unknown space, killing any off-by-one on
  // the accepted-tag ceiling.
  for (int tag = 10; tag <= 255; ++tag) {
    std::string block{'\x01', 'n', static_cast<char>(tag)};
    block.append(16, '\0');  // plenty of value bytes: only the tag can be at fault
    EXPECT_FALSE(DecodeMessage(FrameWithBlock(block)).ok()) << "wire type " << tag;
  }
}

TEST(EventStreamFrameTest, EveryValueShapeTruncatedInsideTheBlockIsAHardError) {
  // Fixed-width tags, one byte short of their width.
  const struct {
    std::uint8_t tag;
    std::size_t width;
    const char* why;
  } kFixed[] = {
      {2, 1, "byte"}, {3, 2, "short"},     {4, 4, "int"},
      {5, 8, "long"}, {8, 8, "timestamp"}, {9, 16, "uuid"},
  };
  for (const auto& v : kFixed) {
    std::string block{'\x01', 'n', static_cast<char>(v.tag)};
    block.append(v.width - 1, '\x7F');
    EXPECT_FALSE(DecodeMessage(FrameWithBlock(block)).ok()) << v.why << " one byte short";
  }
  // Var-length tags: the u16 length itself cut to one byte, and a declared
  // length overrunning the block by exactly one.
  for (const std::uint8_t tag : {std::uint8_t{6}, std::uint8_t{7}}) {
    const std::string half_length{'\x01', 'n', static_cast<char>(tag), '\x00'};
    EXPECT_FALSE(DecodeMessage(FrameWithBlock(half_length)).ok()) << "tag " << int(tag);
    const std::string overrun{'\x01', 'n', static_cast<char>(tag), '\x00', '\x03', 'a', 'b'};
    EXPECT_FALSE(DecodeMessage(FrameWithBlock(overrun)).ok()) << "tag " << int(tag);
  }
}

TEST(EventStreamFrameTest, EncodeRefusesWhatDecodeWouldReject) {
  // The symmetric-bounds contract (ADR-0014).
  const auto empty_name = EncodeMessage(Message{.headers = {{"", true}}});
  EXPECT_FALSE(empty_name.ok());
  const auto long_name = EncodeMessage(Message{.headers = {{std::string(256, 'n'), true}}});
  EXPECT_FALSE(long_name.ok());
  const auto oversized_value =
      EncodeMessage(Message{.headers = {{"v", std::string(0x10000, 'x')}}});
  EXPECT_FALSE(oversized_value.ok());

  // 500 headers of 255-byte values blow the 128 KiB block limit.
  Message block_bomb;
  for (int i = 0; i < 500; ++i) {
    block_bomb.headers.push_back({"h" + std::to_string(i), std::string(255, 'x')});
  }
  EXPECT_FALSE(EncodeMessage(block_bomb).ok());

  const auto payload_bomb =
      EncodeMessage(Message{.payload = Blob(std::vector<std::uint8_t>(kMaxMessageBytes, 0))});
  EXPECT_FALSE(payload_bomb.ok());
}

TEST(EventStreamFrameTest, NameAndValueLengthBoundariesRoundTripAtTheLimit) {
  // The counterpart of the refusals above: the largest legal name and the
  // largest legal values must actually work — an off-by-one that shrank the
  // accepted range would pass every refusal test. One message per boundary:
  // together they would blow the 128 KiB block limit.
  const Message messages[] = {
      Message{.headers = {{std::string(255, 'n'), true}}},
      Message{.headers = {{"s", std::string(0xFFFF, 's')}}},
      Message{.headers = {{"b", Blob(std::vector<std::uint8_t>(0xFFFF, 0xAB))}}},
  };
  for (const Message& message : messages) {
    const std::string frame = EncodeOrDie(message);
    const auto decoded = DecodeOrDie(frame);
    EXPECT_EQ(decoded.message, message);
    EXPECT_EQ(decoded.bytes_consumed, frame.size());
  }

  // One past the u16 value limit refuses on the Blob branch too (the
  // refusal suite above only exercised the string branch).
  EXPECT_FALSE(
      EncodeMessage(Message{.headers = {{"b", Blob(std::vector<std::uint8_t>(0x10000, 0))}}}).ok());
}

TEST(EventStreamFrameTest, HeaderBlockAtExactly128KiBDecodesAndOneMoreByteIsRejected) {
  // Two string headers land the block on exactly 128 KiB:
  //   "a": 1 + 1 + 1 + 2 + 65535 = 65540 bytes
  //   "b": 1 + 1 + 1 + 2 + 65527 = 65532 bytes   -> 131072 total.
  Message message{.headers = {{"a", std::string(65535, 'x')}, {"b", std::string(65527, 'y')}}};
  const std::string frame = EncodeOrDie(message);
  ASSERT_EQ(frame.size(), 16 + kMaxHeaderBlockBytes);
  // The declared headers length is exactly 0x00020000.
  EXPECT_EQ(static_cast<std::uint8_t>(frame[4]), 0x00);
  EXPECT_EQ(static_cast<std::uint8_t>(frame[5]), 0x02);
  EXPECT_EQ(static_cast<std::uint8_t>(frame[6]), 0x00);
  EXPECT_EQ(static_cast<std::uint8_t>(frame[7]), 0x00);
  const auto decoded = DecodeOrDie(frame);
  EXPECT_EQ(decoded.message, message);
  EXPECT_EQ(decoded.bytes_consumed, frame.size());

  // One more value byte crosses the limit: encode refuses...
  std::get<std::string>(message.headers[1].value).push_back('y');
  EXPECT_FALSE(EncodeMessage(message).ok());
  // ...and a prelude declaring headers length 131073 (total 131089) is a
  // decode-side hard error from the 12 prelude bytes alone.
  // python3: struct.pack(">II", 131089, 131073) + zlib CRC.
  const std::vector<std::uint8_t> prelude = {0x00, 0x02, 0x00, 0x11, 0x00, 0x02,
                                             0x00, 0x01, 0xDB, 0xBE, 0x94, 0x8A};
  EXPECT_FALSE(DecodeMessage(BytesToString(prelude)).ok());
}

TEST(EventStreamFrameTest, TotalAtExactly16MiBRoundTripsAndOneByteMoreIsRefused) {
  // A payload sized so the frame is exactly kMaxMessageBytes round-trips...
  const Message at_limit{.payload = Blob(std::vector<std::uint8_t>(kMaxMessageBytes - 16, 0x5A))};
  const std::string frame = EncodeOrDie(at_limit);
  ASSERT_EQ(frame.size(), kMaxMessageBytes);
  const auto decoded = DecodeOrDie(frame);
  EXPECT_EQ(decoded.message.payload.size(), kMaxMessageBytes - 16);
  EXPECT_EQ(decoded.bytes_consumed, kMaxMessageBytes);

  // ...one more payload byte refuses to encode...
  EXPECT_FALSE(
      EncodeMessage(Message{.payload = Blob(std::vector<std::uint8_t>(kMaxMessageBytes - 15, 0))})
          .ok());
  // ...and a prelude declaring total 16777217 is a decode-side hard error
  // from the 12 prelude bytes alone — never a 16 MiB buffering request.
  // python3: struct.pack(">II", 16777217, 0) + zlib CRC.
  const std::vector<std::uint8_t> prelude = {0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
                                             0x00, 0x00, 0x94, 0xE8, 0xF6, 0x47};
  EXPECT_FALSE(DecodeMessage(BytesToString(prelude)).ok());
}

TEST(EventStreamFrameTest, EmptyValuesKeepTheirWireTypeAcrossTheRoundTrip) {
  // Empty is a value, not an absence: the variant alternative must survive.
  const Message message{.headers = {{"s", std::string()}, {"b", Blob()}}};
  const auto decoded = DecodeOrDie(EncodeOrDie(message));
  ASSERT_EQ(decoded.message.headers.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<std::string>(decoded.message.headers[0].value));
  EXPECT_TRUE(std::holds_alternative<Blob>(decoded.message.headers[1].value));
  EXPECT_EQ(decoded.message, message);
}

TEST(EventStreamFrameTest, Int64ExtremesRoundTripAsLongAndAsTimestamp) {
  // Sign-extension pitfalls live at the extremes; long and timestamp share
  // a wire width but must keep their distinct alternatives.
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  const Message message{.headers = {{"lmin", kMin},
                                    {"lmax", kMax},
                                    {"tmin", Timestamp::FromEpochMilliseconds(kMin)},
                                    {"tmax", Timestamp::FromEpochMilliseconds(kMax)}}};
  const auto decoded = DecodeOrDie(EncodeOrDie(message));
  ASSERT_EQ(decoded.message.headers.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<std::int64_t>(decoded.message.headers[0].value));
  EXPECT_TRUE(std::holds_alternative<Timestamp>(decoded.message.headers[2].value));
  EXPECT_EQ(decoded.message, message);
}

TEST(EventStreamFrameTest, PayloadOnlyAndHeadersOnlyFramesRoundTrip) {
  const Message payload_only{.payload = Blob::FromString("just bytes")};
  const std::string payload_frame = EncodeOrDie(payload_only);
  // A payload-only frame declares a zero headers length.
  EXPECT_EQ(
      static_cast<std::uint8_t>(payload_frame[4]) | static_cast<std::uint8_t>(payload_frame[5]) |
          static_cast<std::uint8_t>(payload_frame[6]) | static_cast<std::uint8_t>(payload_frame[7]),
      0);
  const auto decoded_payload = DecodeOrDie(payload_frame);
  EXPECT_EQ(decoded_payload.message, payload_only);

  const Message headers_only{.headers = {{"k", "v"}}};
  const auto decoded_headers = DecodeOrDie(EncodeOrDie(headers_only));
  EXPECT_EQ(decoded_headers.message, headers_only);
  EXPECT_EQ(decoded_headers.message.payload.size(), 0u);
}

TEST(EventStreamFrameTest, DuplicateHeaderNamesAreBothPreservedInOrder) {
  // The codec carries headers; deduplication and ordering policy belong to
  // the protocol layer above it.
  const Message message{.headers = {{"dup", std::int32_t{1}}, {"dup", "two"}}};
  const auto decoded = DecodeOrDie(EncodeOrDie(message));
  ASSERT_EQ(decoded.message.headers.size(), 2u);
  EXPECT_EQ(decoded.message, message);
}

TEST(EventStreamFrameTest, FindHeaderAndFindStringLookUpDecodedHeaders) {
  const Message message{
      .headers = {{":event-type", "chat"}, {"n", 7}, {"dup", "first"}, {"dup", "second"}},
      .payload = Blob::FromString("x")};
  const Message decoded = DecodeOrDie(EncodeOrDie(message)).message;
  // FindString: present-and-string yields the value; a present header of a
  // different wire type and an absent name are both nullptr.
  ASSERT_NE(decoded.FindString(":event-type"), nullptr);
  EXPECT_EQ(*decoded.FindString(":event-type"), "chat");
  EXPECT_EQ(decoded.FindString("n"), nullptr);
  EXPECT_EQ(decoded.FindString("missing"), nullptr);
  // FindHeader: any wire type; duplicates yield the first, in wire order.
  const HeaderValue* n = decoded.FindHeader("n");
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(std::get<std::int32_t>(*n), 7);
  ASSERT_NE(decoded.FindString("dup"), nullptr);
  EXPECT_EQ(*decoded.FindString("dup"), "first");
  EXPECT_EQ(decoded.FindHeader("missing"), nullptr);
}

TEST(EventStreamFrameTest, MessagesRenderThroughTheDebugPrinter) {
  // The house AppendDebugTo convention (issue #85): a failing comparison
  // logged via DebugString reads as structure, not gtest's raw byte dump.
  // Exact output is deliberately unpinned — debug text is not a format.
  Uuid uuid;
  uuid.bytes[0] = 0xAB;
  const Message message{.headers = {{"k", "v"}, {"id", uuid}},
                        .payload = Blob::FromString("payload")};
  const std::string rendered = DebugString(message);
  EXPECT_NE(rendered.find("\"k\""), std::string::npos);
  EXPECT_NE(rendered.find("\"v\""), std::string::npos);
  EXPECT_NE(rendered.find("Uuid(ab"), std::string::npos);
  EXPECT_NE(rendered.find("Blob("), std::string::npos);
}

TEST(EventStreamFrameTest, OneHundredHeadersDecodeInEncodeOrder) {
  Message message;
  for (std::size_t i = 0; i < 100; ++i) {
    message.headers.push_back({"h" + std::to_string(i), static_cast<std::int32_t>(i)});
  }
  const auto decoded = DecodeOrDie(EncodeOrDie(message));
  ASSERT_EQ(decoded.message.headers.size(), 100u);
  for (std::size_t i = 0; i < 100; ++i) {
    EXPECT_EQ(decoded.message.headers[i].name, "h" + std::to_string(i));
    EXPECT_EQ(std::get<std::int32_t>(decoded.message.headers[i].value),
              static_cast<std::int32_t>(i));
  }
}

TEST(EventStreamFrameTest, NamesAndStringValuesAreOpaqueBytesNotValidatedUtf8) {
  // The codec moves bytes; it neither validates nor repairs UTF-8. Invalid
  // sequences and embedded NULs round-trip untouched (validation is the
  // protocol layer's call, and a "helpful" repair would break the CRCs).
  const Message message{
      .headers =
          {
              {std::string("\xFF\xFE name", 7), std::string("\xC3\x28", 2)},
              {std::string("nul\0name", 8), std::string("lone continuation \x80", 19)},
          },
      .payload = Blob::FromString("payload")};
  const auto decoded = DecodeOrDie(EncodeOrDie(message));
  EXPECT_EQ(decoded.message, message);
}

TEST(EventStreamFrameTest, RandomMessagesRoundTripByteExactly) {
  // Deterministic property test: raw mt19937 draws only (the standard pins
  // its output sequence; distributions are implementation-defined) and a
  // literal seed, so every platform runs the same 200 messages.
  std::mt19937 rng(20260719u);
  const auto draw = [&](std::uint32_t bound) { return rng() % bound; };
  const auto draw64 = [&] {
    const auto hi = std::uint64_t{rng()};
    const auto lo = std::uint64_t{rng()};
    return static_cast<std::int64_t>((hi << 32) | lo);
  };
  for (int iteration = 0; iteration < 200; ++iteration) {
    Message message;
    const std::uint32_t header_count = draw(9);
    for (std::uint32_t h = 0; h < header_count; ++h) {
      std::string name;
      const std::uint32_t name_length = 1 + draw(16);
      for (std::uint32_t i = 0; i < name_length; ++i) {
        name.push_back(static_cast<char>(draw(256)));
      }
      HeaderValue value;
      switch (draw(9)) {
        case 0:
          value = draw(2) != 0;
          break;
        case 1:
          value = static_cast<std::int8_t>(draw(256));
          break;
        case 2:
          value = static_cast<std::int16_t>(draw(0x10000));
          break;
        case 3:
          value = static_cast<std::int32_t>(rng());
          break;
        case 4:
          value = draw64();
          break;
        case 5: {
          std::vector<std::uint8_t> bytes(draw(65));
          for (auto& byte : bytes) byte = static_cast<std::uint8_t>(draw(256));
          value = Blob(std::move(bytes));
          break;
        }
        case 6: {
          std::string text(draw(65), '\0');
          for (auto& ch : text) ch = static_cast<char>(draw(256));
          value = std::move(text);
          break;
        }
        case 7:
          value = Timestamp::FromEpochMilliseconds(draw64());
          break;
        default: {
          Uuid uuid;
          for (auto& byte : uuid.bytes) byte = static_cast<std::uint8_t>(draw(256));
          value = uuid;
          break;
        }
      }
      message.headers.push_back({std::move(name), std::move(value)});
    }
    std::vector<std::uint8_t> payload(draw(257));
    for (auto& byte : payload) byte = static_cast<std::uint8_t>(draw(256));
    message.payload = Blob(std::move(payload));

    // Four properties per message: encodable, decodes back equal and fully
    // consumed, re-encodes byte-exactly, and a trailing byte changes
    // nothing.
    const auto frame = EncodeMessage(message);
    ASSERT_TRUE(frame.ok()) << "iteration " << iteration << ": " << frame.error().message();
    const auto decoded = DecodeMessage(*frame);
    ASSERT_TRUE(decoded.ok() && decoded->has_value()) << "iteration " << iteration;
    EXPECT_EQ((*decoded)->message, message) << "iteration " << iteration;
    EXPECT_EQ((*decoded)->bytes_consumed, frame->size()) << "iteration " << iteration;
    const auto reencoded = EncodeMessage((*decoded)->message);
    ASSERT_TRUE(reencoded.ok()) << "iteration " << iteration;
    EXPECT_EQ(*reencoded, *frame) << "iteration " << iteration;
    const auto with_tail = DecodeMessage(*frame + static_cast<char>(draw(256)));
    ASSERT_TRUE(with_tail.ok() && with_tail->has_value()) << "iteration " << iteration;
    EXPECT_EQ((*with_tail)->bytes_consumed, frame->size()) << "iteration " << iteration;
  }
}

}  // namespace
}  // namespace opal::eventstream
