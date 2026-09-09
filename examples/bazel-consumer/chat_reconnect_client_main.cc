// The reconnect hub's CLI client: the generated ChatClient as a
// shell-drivable process, so chat_reconnect_cli_test.sh can script joins,
// vanishes, and rejoins as real processes through the module boundary.
//
// Usage: chat_reconnect_client <port> <name>
//
// stdin, one command per line:
//   /leave      deliberate exit; the hub Removes and closes
//   /quit       close immediately without a leave (the reload/vanish path)
//   <anything>  send it as a Note
//
// stdout, one event per line, flushed for pipes:
//   note <text>              — every received Note
//   closed                   — the stream's clean end; exit 0
//   error <code>: <message>  — a terminal stream error; exit 1

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "acme/chat/client.h"
#include "smithy/client/config.h"
#include "smithy/core/outcome.h"
#include "smithy/eventstream/event_stream.h"

namespace {

void Emit(const std::string& line) { std::cout << line << "\n" << std::flush; }

int Run(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <port> <name>\n", argv[0]);
    return 2;
  }
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.endpoint = "http://127.0.0.1:" + std::string(argv[1]);
  auto client = acme::chat::ChatClient::Create(std::move(config));
  if (!client.ok()) {
    std::fprintf(stderr, "chat-client: %s\n", client.error().message().c_str());
    return 1;
  }

  acme::chat::ExchangeInput input;
  input.name = argv[2];
  auto stream = client->Exchange(input);
  if (!stream.ok()) {
    Emit("error " + stream.error().code() + ": " + stream.error().message());
    return 1;
  }

  // One sender (stdin through an owning handle), one receiver (main).
  std::thread([handle = stream->Share()] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      if (line == "/quit") {
        handle.Close();
        return;
      }
      if (line == "/leave") break;
      if (!handle.Send(acme::chat::Notes::FromNote(acme::chat::Note{.text = line})).ok()) return;
    }
    (void)handle.Send(acme::chat::Notes::FromNote(acme::chat::Note{.text = "/leave"}));
  }).detach();

  while (true) {
    auto received = stream->Receive();
    if (!received.ok()) {
      Emit("error " + received.error().code() + ": " + received.error().message());
      return 1;
    }
    if (!received->has_value()) {
      Emit("closed");
      return 0;
    }
    Emit("note " + (**received).as_note().text);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "chat-client: %s\n", error.what());
    return 1;
  } catch (...) {
    return 1;
  }
}
