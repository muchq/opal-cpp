// A deliberately ill-behaved JSON-RPC peer: dials the stream endpoint in
// raw-text mode and sends stdin verbatim, one text frame per line — no
// codec, no generated client, no manners. The generated TallyClient can
// only speak the wire correctly, so this is how
// jsonrpc_stream_cli_test.sh flexes the server's reserved-code refusals
// and mid-stream policing (ADR-0023) from a shell: whatever a browser
// COULD type at the socket, this peer can.
//
//   jsonrpc_raw_peer <port>
//
// Sends every stdin line first (frames are processed in order, so a
// violation after a valid opening lands mid-stream), then drains the
// socket. stdout, one event per line, flushed for pipes:
//   recv <frame text>  — every received text frame, verbatim
//   closed             — the server's close; exit 0
//   error <message>    — a transport failure instead; exit 1

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "smithy/core/blob.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"

namespace {

void Emit(const std::string& line) { std::cout << line << "\n" << std::flush; }

int Run(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return 2;
  }
  auto socket = opal::http::BeastWebSocketClient::Dial(
      {.host = "127.0.0.1",
       .port = static_cast<std::uint16_t>(std::atoi(argv[1])),
       .raw_text_frames = true});
  if (!socket.ok()) {
    Emit("error " + socket.error().message());
    return 1;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    opal::eventstream::Message frame;
    frame.payload = opal::Blob::FromString(line);
    if (!(*socket)->Send(frame).ok()) break;  // already closed on us: drain below
  }

  while (true) {
    auto received = (*socket)->Receive();
    if (!received.ok()) {
      Emit("error " + received.error().message());
      return 1;
    }
    if (!received->has_value()) {
      Emit("closed");
      return 0;
    }
    Emit("recv " + (**received).payload.ToString());
  }
}

}  // namespace

int main(int argc, char** argv) { return Run(argc, argv); }
