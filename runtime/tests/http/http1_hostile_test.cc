// Hostile-input bank for the HTTP/1.1 message reader, at the pure-parser
// level (no sockets, so unlike socket_transport_hostile_test.cc this runs on
// every platform, byte-exact and timeout-free). The socket-level suite pins
// the server's observable behavior; this bank pins the parser's verdict on
// each framing attack individually, including client-side response framing
// that no socket test drives.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "opal/http/http1.h"

namespace opal::http {
namespace {

// Reads from an in-memory wire capture, `chunk` bytes per call, EOF after.
Http1ReadFn FromString(std::string wire, std::size_t chunk = 8192) {
  auto offset = std::make_shared<std::size_t>(0);
  auto data = std::make_shared<std::string>(std::move(wire));
  return [offset, data, chunk](char* buffer, std::size_t capacity) -> long {
    const std::size_t want = capacity < chunk ? capacity : chunk;
    const std::size_t left = data->size() - *offset;
    const std::size_t take = want < left ? want : left;
    if (take == 0) return 0;
    data->copy(buffer, take, *offset);
    *offset += take;
    return static_cast<long>(take);
  };
}

Outcome<Http1Message> Parse(const std::string& wire, bool body_until_eof = false,
                            std::size_t chunk = 8192) {
  return ReadHttp1Message(FromString(wire, chunk), body_until_eof);
}

TEST(Http1HostileTest, RejectsSmugglingFraming) {
  const struct {
    const char* wire;
    const char* why;
  } bank[] = {
      {"POST / HTTP/1.1\r\ncontent-length: 4\r\ncontent-length: 4\r\n\r\nabcd",
       "duplicate content-length, even when they agree"},
      {"POST / HTTP/1.1\r\ncontent-length: 4\r\ncontent-length: 11\r\n\r\nabcd",
       "conflicting content-length"},
      {"POST / HTTP/1.1\r\nContent-Length: 4\r\ncontent-LENGTH: 11\r\n\r\nabcd",
       "conflicting content-length across case spellings"},
      {"POST / HTTP/1.1\r\ntransfer-encoding: chunked\r\n\r\n0\r\n\r\n",
       "chunked transfer-encoding is unsupported"},
      {"POST / HTTP/1.1\r\ntransfer-encoding: identity\r\n\r\n",
       "any transfer-encoding is unsupported"},
      {"POST / HTTP/1.1\r\ncontent-length: 4\r\ntransfer-encoding: chunked\r\n\r\nabcd",
       "TE + CL is the classic desync"},
  };
  for (const auto& c : bank) {
    EXPECT_FALSE(Parse(c.wire).ok()) << c.why;
  }
}

TEST(Http1HostileTest, RejectsHostileContentLengths) {
  const struct {
    const char* wire;
    const char* why;
  } bank[] = {
      {"POST / HTTP/1.1\r\ncontent-length: abc\r\n\r\n", "non-numeric"},
      {"POST / HTTP/1.1\r\ncontent-length: 4x\r\n\r\nabcd", "trailing junk"},
      {"POST / HTTP/1.1\r\ncontent-length: 0x10\r\n\r\n", "hex"},
      {"POST / HTTP/1.1\r\ncontent-length: 4 4\r\n\r\nabcd", "embedded space"},
      {"POST / HTTP/1.1\r\ncontent-length: -1\r\n\r\n", "negative"},
      {"POST / HTTP/1.1\r\ncontent-length: +4\r\n\r\nabcd", "plus-signed"},
      {"POST / HTTP/1.1\r\ncontent-length: 67108865\r\n\r\n", "one over the 64 MiB cap"},
      {"POST / HTTP/1.1\r\ncontent-length: 18446744073709551617\r\n\r\n",
       "2^64+1 overflows strtoull; must reject, not truncate"},
      {"POST / HTTP/1.1\r\ncontent-length: 99999999999999999999\r\n\r\n",
       "over-uint64 must not become a small number"},
      {"POST / HTTP/1.1\r\ncontent-length:\r\n\r\n", "empty value"},
  };
  for (const auto& c : bank) {
    EXPECT_FALSE(Parse(c.wire).ok()) << c.why;
  }
}

TEST(Http1HostileTest, RejectsMalformedHeaderBlocks) {
  const struct {
    const char* wire;
    const char* why;
  } bank[] = {
      {"GET / HTTP/1.1\r\nno-colon-here\r\n\r\n", "header line without a colon"},
      {"GET / HTTP/1.1\r\n continued\r\n\r\n", "obs-fold continuation line"},
      {"GET / HTTP/1.1\r\nx: 1\r\ngarbage\r\n\r\n", "later line without a colon"},
  };
  for (const auto& c : bank) {
    EXPECT_FALSE(Parse(c.wire).ok()) << c.why;
  }
}

TEST(Http1HostileTest, RejectsTruncationEverywhere) {
  // Any strict prefix of a complete request must fail (headers never
  // terminate) or report a mid-body close — never a parsed message.
  const std::string full = "POST /x HTTP/1.1\r\nx-a: 1\r\ncontent-length: 4\r\n\r\nabcd";
  for (std::size_t cut = 0; cut < full.size(); ++cut) {
    EXPECT_FALSE(Parse(full.substr(0, cut)).ok()) << "cut at " << cut;
  }
}

TEST(Http1HostileTest, RejectsOversizedSections) {
  // Headers past the 64 KiB cap.
  std::string flood = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 3000; ++i) flood += "x-h" + std::to_string(i) + ": aaaaaaaaaaaaaaaa\r\n";
  flood += "\r\n";
  EXPECT_FALSE(Parse(flood).ok()) << "header flood";

  // Excess body bytes beyond the declared length.
  EXPECT_FALSE(Parse("POST / HTTP/1.1\r\ncontent-length: 2\r\n\r\nabcd").ok())
      << "body longer than content-length";

  // Read-error propagation.
  EXPECT_FALSE(ReadHttp1Message([](char*, std::size_t) -> long { return -1; }, false).ok());
  EXPECT_FALSE(Parse("").ok()) << "immediate EOF";
}

TEST(Http1HostileTest, AcceptsStrictButUglyMessages) {
  // Exact content-length, byte-at-a-time delivery.
  auto message = Parse("POST /x HTTP/1.1\r\ncontent-length: 4\r\n\r\nabcd", false, 1);
  ASSERT_TRUE(message.ok());
  EXPECT_EQ(message->start_line, "POST /x HTTP/1.1");
  EXPECT_EQ(message->body, "abcd");

  // Tab/space padding trims; header lookup is case-insensitive; empty values
  // and repeated non-framing headers are legal.
  message =
      Parse("GET / HTTP/1.1\r\nX-Padded: \t v \t \r\nx-empty:\r\nx-dup: a\r\nx-dup: b\r\n\r\n");
  ASSERT_TRUE(message.ok());
  EXPECT_EQ(message->headers.Get("x-padded"), "v");
  EXPECT_EQ(message->headers.Get("x-empty"), "");
  EXPECT_EQ(message->headers.GetAll("x-dup").size(), 2u);

  // Response framing: no content-length, body extends to EOF.
  message = Parse("HTTP/1.1 200 OK\r\nx: 1\r\n\r\npayload beyond headers", true, 3);
  ASSERT_TRUE(message.ok());
  EXPECT_EQ(message->body, "payload beyond headers");

  // Same message read server-side (no EOF body): the body is empty.
  message = Parse("HTTP/1.1 200 OK\r\nx: 1\r\n\r\n", false);
  ASSERT_TRUE(message.ok());
  EXPECT_TRUE(message->body.empty());

  // content-length: 0 with nothing after it.
  message = Parse("POST / HTTP/1.1\r\ncontent-length: 0\r\n\r\n");
  ASSERT_TRUE(message.ok());
  EXPECT_TRUE(message->body.empty());
}

TEST(Http1HostileTest, StartLineHelpersMatchTheTransports) {
  std::string method;
  std::string target;
  ASSERT_TRUE(ParseRequestLine("GET /a/b?q=1 HTTP/1.1", &method, &target));
  EXPECT_EQ(method, "GET");
  EXPECT_EQ(target, "/a/b?q=1");
  EXPECT_FALSE(ParseRequestLine("GET", &method, &target));
  EXPECT_FALSE(ParseRequestLine("GET /only-one-space", &method, &target));
  EXPECT_FALSE(ParseRequestLine("", &method, &target));

  auto status = ParseStatusLine("HTTP/1.1 200 OK");
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(*status, 200);
  EXPECT_EQ(*ParseStatusLine("HTTP/1.1 599 "), 599);
  EXPECT_FALSE(ParseStatusLine("HTTP/1.1 007 James").ok()) << "sub-100 status";
  EXPECT_FALSE(ParseStatusLine("HTTP/1.1 999 nope").ok()) << "over-599 status";
  EXPECT_FALSE(ParseStatusLine("ICY 200 OK").ok()) << "not HTTP";
  EXPECT_FALSE(ParseStatusLine("HTTP/1.1").ok()) << "no space";
  EXPECT_FALSE(ParseStatusLine("").ok());
}

}  // namespace
}  // namespace opal::http
