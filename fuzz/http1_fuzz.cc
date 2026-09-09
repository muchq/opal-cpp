// Fuzz target: the hand-rolled HTTP/1.1 message reader behind the socket
// transports (issue #48 — the one network-facing parser that had no fuzz
// coverage). The input's first byte picks the read mode and a chunking
// pattern; the rest is the wire stream, delivered in varying slices so the
// incremental header/body accumulation paths get exercised, not just the
// all-at-once happy path. The parser must never crash, over-read, or accept
// a body larger than its documented cap; the start-line helpers must never
// crash on arbitrary bytes.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "smithy/http/http1.h"

namespace {

constexpr std::size_t kMaxBodyBytes = std::size_t{64} * 1024 * 1024;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  const std::uint8_t control = data[0];
  const bool body_until_eof = (control & 1) != 0;
  // Chunk sizes cycle through a pattern seeded by the control byte: sizes of
  // 1 stress byte-at-a-time accumulation, larger ones the buffered path.
  const std::size_t patterns[4][3] = {{1, 1, 1}, {1, 7, 4096}, {3, 8192, 2}, {8192, 8192, 8192}};
  const std::size_t* chunk_sizes = patterns[(control >> 1) & 3];

  const char* wire = reinterpret_cast<const char*>(data + 1);
  std::size_t remaining = size - 1;
  std::size_t offset = 0;
  int call = 0;
  const auto read = [&](char* buffer, std::size_t capacity) -> long {
    const std::size_t want = std::min(capacity, chunk_sizes[call++ % 3]);
    const std::size_t take = std::min(want, remaining - offset);
    if (take == 0) return 0;  // EOF
    std::memcpy(buffer, wire + offset, take);
    offset += take;
    return static_cast<long>(take);
  };

  auto message = opal::http::ReadHttp1Message(read, body_until_eof);
  if (message.ok()) {
    if (message->body.size() > kMaxBodyBytes) std::abort();
    // Whatever parsed must be internally consistent enough to iterate.
    for (const auto& [name, value] : message->headers.entries()) {
      if (name.find("\r\n") != std::string::npos) std::abort();
      (void)value;
    }
    std::string method;
    std::string target;
    (void)opal::http::ParseRequestLine(message->start_line, &method, &target);
    (void)opal::http::ParseStatusLine(message->start_line);
  }
  return 0;
}
