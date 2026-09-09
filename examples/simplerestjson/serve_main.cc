// The serving lifecycle example: SIGTERM/SIGINT → drained shutdown.
// docs/production-guide.md § "Serving lifecycle" is the walkthrough (and
// mirrors main() below byte-for-byte). Try it:
//
//   bazel run //examples/simplerestjson:bookstore_server
//   curl localhost:8080/books -H 'content-type: application/json' -d '{"isbn":"1","title":"Dune"}'
//   curl localhost:8080/books/1
//   kill -TERM <pid>   # or Ctrl-C: drains in-flight requests, then exits 0

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "example/bookstore/server.h"
#include "example/bookstore/types.h"
#include "smithy/core/outcome.h"
#include "smithy/http/beast_transport.h"

namespace {

using example::bookstore::AddBookInput;
using example::bookstore::AddBookOutput;
using example::bookstore::BookNotFound;
using example::bookstore::BookstoreHandler;
using example::bookstore::BookstoreServer;
using example::bookstore::GetBookInput;
using example::bookstore::GetBookOutput;

// Handlers must be thread-safe (server-guide.md): the transport dispatches
// on a thread pool.
class InMemoryBookstore final : public BookstoreHandler {
 public:
  opal::Outcome<AddBookOutput> AddBook(const AddBookInput& input,
                                       const opal::server::RequestContext& /*context*/) override {
    const std::lock_guard<std::mutex> lock(mu_);
    titles_[input.isbn] = input.title;
    return AddBookOutput{.status = 201, .isbn = input.isbn};
  }

  opal::Outcome<GetBookOutput> GetBook(const GetBookInput& input,
                                       const opal::server::RequestContext& /*context*/) override {
    const std::lock_guard<std::mutex> lock(mu_);
    const auto it = titles_.find(input.isbn);
    if (it == titles_.end()) {
      const std::string message = "no book: " + input.isbn;
      opal::Error error = opal::Error::Modeled("BookNotFound", message);
      error.set_detail(BookNotFound{.message = message, .isbn = input.isbn});
      return error;  // the server turns this into the modeled 404
    }
    return GetBookOutput{.isbn = input.isbn, .title = it->second};
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::string> titles_;
};

}  // namespace

// [production-guide:main]
int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  // Before Start(): threads the transport creates inherit this mask, so the
  // shutdown signals reach only the sigwait() below.
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  BookstoreServer server(std::make_shared<InMemoryBookstore>());
  opal::http::BeastServerTransport transport({
      .address = "0.0.0.0",
      .port = argc > 1 ? std::atoi(argv[1]) : 8080,  // 0 binds an ephemeral port
      .drain_timeout_seconds = 10,
  });
  opal::Outcome<opal::Unit> started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "bookstore: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "bookstore: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);  // serve until SIGTERM/SIGINT
  std::fprintf(stderr, "bookstore: signal %d, draining\n", signal_number);
  transport.Stop();  // in-flight requests get drain_timeout_seconds to finish
  return 0;
}
// [/production-guide:main]
