// The hub's CLI client (issue #112): a generated ChatClient turned into a
// shell-drivable process, so hub_cli_test.sh can run several of these
// against one hub_server and script a conversation. It is also the client-
// side Share() demo: the stdin thread sends through an owning
// EventStreamHandle while main runs the canonical receive loop — when the
// stream ends and main returns, the detached thread's handle fails softly
// instead of dangling.
//
// Usage: hub_client <port> <room> <nickname>     — join the conversation
//        hub_client <port> <room> --watch        — observe only
//
// stdin (conversation mode), one command per line:
//   /leave      send a LeaveNotice; the hub says goodbye and closes
//   /quit       close the session immediately (the vanish path)
//   <anything>  send it as a ChatMessage
// (stdin reaching EOF behaves like /leave.)
//
// stdout, one event per line, flushed for pipes:
//   joined <member> | message <sender> <text> | left <member>
//   closed                       — the stream's clean end; exit 0
//   error <code>: <message>      — a terminal stream error; exit 1

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "example/chat/client.h"
#include "opal/client/config.h"
#include "opal/core/outcome.h"
#include "opal/eventstream/event_stream.h"

namespace {

// One line out, flushed — the shell test reads these through a pipe.
void Emit(const std::string& line) { std::cout << line << "\n" << std::flush; }

void EmitError(const opal::Error& error) { Emit("error " + error.code() + ": " + error.message()); }

void PrintEvent(const example::chat::RoomEvents& event) {
  if (event.is_joined()) {
    Emit("joined " + event.as_joined().member);
  } else if (event.is_message()) {
    Emit("message " + event.as_message().sender.value_or("?") + " " + event.as_message().text);
  } else if (event.is_left()) {
    Emit("left " + event.as_left().member);
  }
}

// The canonical consume loop (server-guide.md), shared by both modes: print
// until the stream ends, report how.
template <typename Stream>
int Serve(Stream& stream) {
  while (true) {
    auto received = stream.Receive();
    if (!received.ok()) {
      EmitError(received.error());
      return 1;
    }
    const auto& event = *received;
    if (!event.has_value()) {
      Emit("closed");
      return 0;
    }
    PrintEvent(*event);
  }
}

int Run(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <port> <room> <nickname|--watch>\n", argv[0]);
    return 2;
  }
  const int port = std::atoi(argv[1]);
  const std::string room = argv[2];
  const std::string name = argv[3];

  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.endpoint = "http://127.0.0.1:" + std::to_string(port);
  auto client = example::chat::ChatClient::Create(std::move(config));
  if (!client.ok()) {
    std::fprintf(stderr, "hub-client: %s\n", client.error().message().c_str());
    return 1;
  }

  if (name == "--watch") {
    example::chat::WatchInput input;
    input.room = room;
    auto stream = client->Watch(input);
    if (!stream.ok()) {
      EmitError(stream.error());
      return 1;
    }
    return Serve(*stream);
  }

  example::chat::ConverseInput input;
  input.room = room;
  input.nickname = name;
  auto stream = client->Converse(input);
  if (!stream.ok()) {
    EmitError(stream.error());
    return 1;
  }

  // One sender, one receiver (the WebSocket threading contract): stdin
  // drives sends through an owning handle, main consumes. Detaching is safe
  // because the handle outlives the stream by design — after main's Serve
  // returns and the stream dies, a straggling Send just reports Transport.
  std::thread([handle = stream->Share()] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      if (line == "/quit") {
        handle.Close();
        return;
      }
      if (line == "/leave") break;
      example::chat::ChatMessage message;
      message.text = line;
      if (!handle.Send(example::chat::ChatEvents::FromMessage(message)).ok()) return;
    }
    // /leave or stdin closed: say goodbye; the hub answers with its own
    // goodbye and the close.
    (void)handle.Send(example::chat::ChatEvents::FromLeave(example::chat::LeaveNotice{}));
  }).detach();

  return Serve(*stream);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "hub-client: %s\n", error.what());
    return 1;
  } catch (...) {
    return 1;
  }
}
