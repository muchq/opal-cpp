// Hostile HTTP/1.1 framing against SocketHttpServer's hand-rolled parser
// (ReadMessage). SocketHttpClient only ever emits well-formed requests, so
// these drive raw bytes down a socket to exercise the paths a real attacker
// controls: request-smuggling framing, malformed content-lengths, header
// floods, truncation. The invariant is that the server never crashes, never
// hangs, and never mis-parses ambiguous framing as a successful request.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <optional>
#include <string>

#include "smithy/http/socket_transport.h"

namespace opal::http {
namespace {

// Sends raw bytes to 127.0.0.1:port, half-closes so the server sees EOF (no
// waiting on the read timeout), and returns whatever the server wrote back —
// or nullopt if the connection could not be made. An empty string means the
// server closed without a response.
std::optional<std::string> RawExchange(int port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  // Bound the whole exchange so a parser bug surfaces as a test timeout on
  // this socket, not a hung test binary.
  timeval tv{};
  tv.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const char* data = request.data();
  std::size_t remaining = request.size();
  while (remaining > 0) {
    const auto sent = ::send(fd, data, remaining, MSG_NOSIGNAL);
    if (sent <= 0) break;
    data += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  ::shutdown(fd, SHUT_WR);  // signal end-of-request

  std::string response;
  char buffer[4096];
  while (true) {
    const auto got = ::recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) break;
    response.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return response;
}

// Parses the status code from an "HTTP/1.1 NNN ..." response, or nullopt if the
// server sent nothing / something unparseable (a rejection, which is fine).
std::optional<int> StatusOf(const std::optional<std::string>& response) {
  if (!response || response->rfind("HTTP/", 0) != 0) return std::nullopt;
  const auto space = response->find(' ');
  if (space == std::string::npos) return std::nullopt;
  return std::atoi(response->c_str() + space + 1);
}

class HostileFramingTest : public testing::Test {
 protected:
  void SetUp() override {
    // A 200-returning handler: if hostile framing were mis-parsed as a valid
    // request, we would observe 200 — which every case below forbids.
    ASSERT_TRUE(server_.Start([](const HttpRequest&) { return HttpResponse{200, {}, "ok"}; }).ok());
  }
  void TearDown() override { server_.Stop(); }

  SocketHttpServer server_;
};

TEST_F(HostileFramingTest, RejectsConflictingContentLength) {
  // The body exactly matches the first content-length, so nothing but the
  // explicit duplicate-header rejection stops this: a proxy honoring the
  // second length and this server honoring the first is the smuggling desync.
  const auto response = RawExchange(
      server_.port(),
      "POST / HTTP/1.1\r\nhost: x\r\ncontent-length: 5\r\ncontent-length: 6\r\n\r\nhello");
  const auto status = StatusOf(response);
  // Ambiguous framing must never be accepted as a valid request.
  EXPECT_NE(status.value_or(400), 200);
}

TEST_F(HostileFramingTest, RejectsTransferEncoding) {
  const auto response = RawExchange(
      server_.port(),
      "POST / HTTP/1.1\r\nhost: x\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
  EXPECT_NE(StatusOf(response).value_or(400), 200);
}

TEST_F(HostileFramingTest, RejectsNegativeContentLength) {
  const auto response =
      RawExchange(server_.port(), "POST / HTTP/1.1\r\nhost: x\r\ncontent-length: -1\r\n\r\n");
  EXPECT_NE(StatusOf(response).value_or(400), 200);
}

TEST_F(HostileFramingTest, RejectsNonNumericContentLength) {
  const auto response =
      RawExchange(server_.port(), "POST / HTTP/1.1\r\nhost: x\r\ncontent-length: abc\r\n\r\n");
  EXPECT_NE(StatusOf(response).value_or(400), 200);
}

TEST_F(HostileFramingTest, RejectsOverflowingContentLength) {
  const auto response =
      RawExchange(server_.port(),
                  "POST / HTTP/1.1\r\nhost: x\r\ncontent-length: 999999999999999999999999\r\n\r\n");
  EXPECT_NE(StatusOf(response).value_or(400), 200);
}

TEST_F(HostileFramingTest, SurvivesHeaderFlood) {
  // Thousands of headers, kept just under the 64 KiB header cap. Must not
  // crash or hang; whether it 200s or 400s is not the point.
  std::string request = "GET / HTTP/1.1\r\nhost: x\r\n";
  for (int i = 0; i < 2000 && request.size() < 60 * 1024; ++i) {
    request += "x-pad-" + std::to_string(i) + ": v\r\n";
  }
  request += "\r\n";
  const auto response = RawExchange(server_.port(), request);
  // Reached here without the server dying — that is the assertion. A response
  // is expected but its code is unconstrained.
  EXPECT_TRUE(response.has_value());
}

TEST_F(HostileFramingTest, HandlesTruncatedHeadersWithoutHanging) {
  // Headers that never terminate, then EOF. The server must give up (bounded
  // by the half-close) rather than block forever.
  const auto response = RawExchange(server_.port(), "GET / HTTP/1.1\r\nhost: x\r\nx-partial: ");
  EXPECT_NE(StatusOf(response).value_or(400), 200);
}

TEST_F(HostileFramingTest, StillServesAWellFormedRequestAfterward) {
  // Sanity: the hardening didn't break the normal path.
  const auto response =
      RawExchange(server_.port(), "GET / HTTP/1.1\r\nhost: x\r\ncontent-length: 0\r\n\r\n");
  EXPECT_EQ(StatusOf(response).value_or(0), 200);
}

TEST(HostileFramingResponseTest, ServerEmitsExactlyOneContentLength) {
  // Regression: a handler that sets its own content-length (generated servers
  // do this for payload responses) must not cause the transport to emit a
  // second one — a duplicate a strict client now rejects.
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response{200, {}, "body"};
                    response.headers.Set("content-length", "4");
                    return response;
                  })
                  .ok());
  const auto response =
      RawExchange(server.port(), "GET / HTTP/1.1\r\nhost: x\r\ncontent-length: 0\r\n\r\n");
  server.Stop();
  ASSERT_TRUE(response.has_value());
  // Count content-length header lines in the response head.
  int count = 0;
  std::size_t pos = 0;
  const std::string needle = "content-length:";
  while ((pos = response->find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  EXPECT_EQ(count, 1) << *response;
}

}  // namespace
}  // namespace opal::http
