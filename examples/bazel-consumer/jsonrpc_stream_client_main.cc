// The tally's CLI client: the generated TallyClient as a shell-drivable
// process, so jsonrpc_stream_cli_test.sh can script whole sessions on the
// JSON-RPC stream wire (ADR-0023) as real processes through the module
// boundary. The dial, the opening envelope, and the terminal handling are
// all the generated surface — nothing hand-rolled here.
//
// Usage: jsonrpc_stream_client <port> <start> <bump>...
//
// Sends each bump in order, printing every received total; a zero bump is
// the server's clean-end cue.
//
// stdout, one event per line, flushed for pipes:
//   total <n>                — every received running total
//   closed                   — the stream's clean end; exit 0
//   error <code>: <message>  — a terminal stream error; exit 1

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "acme/tally/client.h"
#include "opal/client/config.h"
#include "opal/core/outcome.h"

namespace {

void Emit(const std::string& line) { std::cout << line << "\n" << std::flush; }

int Run(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <port> <start> <bump>...\n", argv[0]);
    return 2;
  }
  opal::ClientConfig config;
  config.retry.max_attempts = 1;
  config.endpoint = "http://127.0.0.1:" + std::string(argv[1]);
  auto client = acme::tally::TallyClient::Create(std::move(config));
  if (!client.ok()) {
    std::fprintf(stderr, "tally-client: %s\n", client.error().message().c_str());
    return 1;
  }

  acme::tally::CountInput input;
  input.start = std::atoi(argv[2]);
  auto stream = client->Count(input);
  if (!stream.ok()) {
    Emit("error " + stream.error().code() + ": " + stream.error().message());
    return 1;
  }

  // Bumps go out up front (sends and receives interleave freely on a full
  // duplex session); the loop below then drains what comes back.
  for (int i = 3; i < argc; ++i) {
    if (!stream->Send(acme::tally::Bumps::FromBump(acme::tally::Bump{.by = std::atoi(argv[i])}))
             .ok()) {
      break;
    }
  }

  while (true) {
    auto total = stream->Receive();
    if (!total.ok()) {
      Emit("error " + total.error().code() + ": " + total.error().message());
      return 1;
    }
    if (!total->has_value()) {
      Emit("closed");
      return 0;
    }
    Emit("total " + std::to_string((**total).as_total().value));
  }
}

}  // namespace

int main(int argc, char** argv) { return Run(argc, argv); }
