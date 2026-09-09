// The reconnect hub on the GENERATED async surface (ADR-0021), out of
// tree: chat_reconnect_server_main.cc's exact wire behavior — join
// broadcasts, snapshot resume, grace expiry, /leave, drain — served with
// zero parked threads by the generated ChatAsyncHandler instead of the
// blocking ChatHandler. chat_async_reconnect_cli_test reuses the blocking
// suite's script verbatim against this binary: same phases passing on
// both seams is the cross-seam parity proof through the module boundary.
//
//   chat_async_reconnect_server [port [grace-seconds]]

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "acme/chat/server.h"
#include "opal/core/outcome.h"
#include "opal/http/beast_transport.h"
#include "opal/server/session_registry.h"

namespace {

using acme::chat::ExchangeAsyncServerStream;
using acme::chat::ExchangeInput;
using acme::chat::Note;
using acme::chat::Notes;

using Registry = opal::server::SessionRegistry<Notes>;

// The generated async handler with the reconnect exits split the ADR-0020
// way: deliberate leaves Remove, everything else Detaches. Delivery rides
// the async registry — no writer threads, no parked handler threads.
class AsyncReconnectHubHandler final : public acme::chat::ChatAsyncHandler {
 public:
  explicit AsyncReconnectHubHandler(std::chrono::seconds grace)
      : registry_(RegistryOptions(this, grace)) {}

  opal::eventstream::StreamTask Exchange(ExchangeInput input,
                                         ExchangeAsyncServerStream& stream) override {
    const std::string id = input.name;
    // The blessed admission call (ADR-0022): resume-or-fresh-join with the
    // brief retry the reconnect race needs. It blocks, legally: this runs
    // pre-first-suspend, on the launching handler thread.
    const auto admission =
        registry_.ResumeOrAdd(id, [&stream] { return stream.Share(); }, std::chrono::seconds(1));
    if (admission == Registry::Admission::kRefused) {
      // The generated wrapper frames this as the typed exception message.
      co_return opal::Error::Validation("name '" + id + "' is already in the session");
    }

    if (admission == Registry::Admission::kResumed) {
      // Snapshot replay, the blessed recovery: current authoritative
      // state as this session's first event — never missed messages.
      std::string roster;
      for (const std::string& member : registry_.Ids()) {
        roster += (roster.empty() ? "" : ",") + member;
      }
      (void)co_await stream.Send(Notes::FromNote(Note{.text = "snapshot:" + roster}));
    } else {
      BroadcastText("joined:" + id);
    }

    bool left = false;
    while (true) {
      auto event = co_await stream.Receive();
      if (!event.ok() || !event->has_value()) break;
      const std::string& text = (**event).as_note().text;
      if (text == "/leave") {
        left = true;
        break;
      }
      BroadcastText(id + ":" + text);
    }

    if (left) {
      registry_.Remove(id);
      BroadcastText("left:" + id);  // the others; the leaver is deregistered
      // The leaver's own goodbye, directly — the request/reply moment.
      (void)co_await stream.Send(Notes::FromNote(Note{.text = "left:" + id}));
    } else if (!registry_.Detach(id)) {
      // Grace disabled or the entry already gone: the immediate path.
      registry_.Remove(id);
      BroadcastText("left:" + id);
    }
    co_return opal::Unit{};  // the generated wrapper closes the stream
  }

  bool Drain(std::chrono::milliseconds timeout) { return registry_.Drain(timeout); }

 private:
  void BroadcastText(const std::string& text) {
    registry_.Broadcast(Notes::FromNote(Note{.text = text}));
  }

  static opal::server::SessionRegistry<Notes>::Options RegistryOptions(
      AsyncReconnectHubHandler* handler, std::chrono::seconds grace) {
    opal::server::SessionRegistry<Notes>::Options options;
    options.async_delivery = true;  // chains on the transport's completions
    options.grace_period = grace;
    options.on_expired = [handler](const std::string& id) {
      // Nobody came back: the departure the disconnect deferred.
      handler->BroadcastText("left:" + id);
    };
    return options;
  }

  opal::server::SessionRegistry<Notes> registry_;
};

}  // namespace

int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  const std::chrono::seconds grace{argc > 2 ? std::atoi(argv[2]) : 300};
  auto handler = std::make_shared<AsyncReconnectHubHandler>(grace);
  // The async constructor (ADR-0021): the streaming route registers on the
  // shared-session seam; sessions park no handler thread.
  acme::chat::ChatServer server(handler);

  opal::http::BeastServerTransport::Options options;
  options.address = "0.0.0.0";
  options.port = argc > 1 ? std::atoi(argv[1]) : 8080;  // 0 binds an ephemeral port
  options.handler_threads = 2;                          // launches only — sessions hold no thread
  options.websocket_gate = server.StreamRouter()->Gate();
  options.on_websocket_session = server.StreamRouter()->ServeSession();
  opal::http::BeastServerTransport transport(options);
  auto started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "chat-hub: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "chat-hub: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);
  std::fprintf(stderr, "chat-hub: signal %d, draining\n", signal_number);
  const bool drained = handler->Drain(std::chrono::seconds(5));
  std::fprintf(stderr, drained ? "chat-hub: drained\n" : "chat-hub: drain timed out\n");
  transport.Stop();
  return drained ? 0 : 1;
}
