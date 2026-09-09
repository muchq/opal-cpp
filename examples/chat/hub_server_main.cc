// The consumer hub over real WebSockets (issue #112): HubHandler behind
// BeastServerTransport, with the graceful shutdown the SessionRegistry
// makes one line — SIGTERM/SIGINT → Drain() (close every live session, wait
// for the handlers to unwind) → Stop(). hub_cli_test.sh drives this binary
// together with hub_client_main.cc as real processes. Try it:
//
//   bazel run //examples/chat:hub_server
//   bazel run //examples/chat:hub_client -- 8080 lobby ada     # per terminal
//   kill -TERM <pid>   # or Ctrl-C: drains the hub, then exits 0

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "example/chat/server.h"
#include "hub_handler.h"
#include "smithy/core/outcome.h"
#include "smithy/http/beast_transport.h"

int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  // Before Start(): threads the transport creates inherit this mask, so the
  // shutdown signals reach only the sigwait() below.
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  auto handler = std::make_shared<example::chat::HubHandler>();
  example::chat::ChatServer server(handler);
  opal::http::BeastServerTransport::Options options;
  options.address = "0.0.0.0";
  options.port = argc > 1 ? std::atoi(argv[1]) : 8080;  // 0 binds an ephemeral port
  options.websocket_gate = server.StreamRouter()->Gate();
  options.on_websocket = server.StreamRouter()->Serve();
  opal::http::BeastServerTransport transport(options);
  opal::Outcome<opal::Unit> started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "chat-hub: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "chat-hub: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);  // serve until SIGTERM/SIGINT
  std::fprintf(stderr, "chat-hub: signal %d, draining %zu session(s)\n", signal_number,
               handler->sessions());
  // Proposal 3's one-liner: close every session and wait for the handlers to
  // unwind, so the transport's abort-flavored Stop() finds nothing to abort.
  const bool drained = handler->Drain(std::chrono::seconds(5));
  std::fprintf(stderr, drained ? "chat-hub: drained\n"
                               : "chat-hub: drain timed out; aborting remaining sessions\n");
  transport.Stop();
  return drained ? 0 : 1;
}
