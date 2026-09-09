// The jsonRpc2 stream wire (ADR-0023) as a real consumer process: the
// GENERATED Tally service on its async surface (ADR-0021), served by
// BeastServerTransport in raw-text mode — the one flag beyond the ADR-0016
// two-liner a jsonRpc2 streaming server needs. jsonrpc_stream_cli_test.sh
// drives it with the generated CLI client through the module boundary.
//
//   jsonrpc_stream_server [port]

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "acme/tally/server.h"
#include "smithy/core/outcome.h"
#include "smithy/http/beast_transport.h"

namespace {

using acme::tally::Bump;
using acme::tally::Busted;
using acme::tally::CountAsyncServerStream;
using acme::tally::CountInput;
using acme::tally::Total;
using acme::tally::Totals;

class TallyHandler final : public acme::tally::TallyAsyncHandler {
 public:
  opal::eventstream::StreamTask Count(CountInput input, CountAsyncServerStream& stream) override {
    int total = input.start.value_or(0);
    while (true) {
      auto bump = co_await stream.Receive();
      if (!bump.ok() || !bump->has_value()) co_return opal::Unit{};
      const int by = (**bump).as_bump().by;
      if (by == 0) co_return opal::Unit{};  // the terminal result, then the close
      total += by;
      if (total < 0) {
        // The terminal error envelope, typed all the way to the client.
        opal::Error busted = opal::Error::Modeled("Busted", "the tally went negative");
        busted.set_detail(Busted{.message = "the tally went negative"});
        co_return busted;
      }
      auto sent = co_await stream.Send(Totals::FromTotal(Total{.value = total}));
      if (!sent.ok()) co_return opal::Unit{};
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  // The async constructor (ADR-0021): the shared-endpoint stream route
  // registers on the shared-session seam; sessions park no handler thread.
  acme::tally::TallyServer server(std::make_shared<TallyHandler>());

  opal::http::BeastServerTransport::Options options;
  options.address = "0.0.0.0";
  options.port = argc > 1 ? std::atoi(argv[1]) : 8080;  // 0 binds an ephemeral port
  options.handler_threads = 2;                          // launches only
  options.websocket_gate = server.StreamRouter()->Gate();
  options.on_websocket_session = server.StreamRouter()->ServeSession();
  options.websocket_raw_text_frames = true;  // the JSON-RPC text wire (ADR-0023)
  opal::http::BeastServerTransport transport(options);
  auto started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "tally: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "tally: serving on :%d (SIGTERM or Ctrl-C exits)\n", transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);
  std::fprintf(stderr, "tally: signal %d, stopping\n", signal_number);
  transport.Stop();
  return 0;
}
