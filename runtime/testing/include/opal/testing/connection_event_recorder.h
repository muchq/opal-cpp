#ifndef OPAL_TESTING_CONNECTION_EVENT_RECORDER_H_
#define OPAL_TESTING_CONNECTION_EVENT_RECORDER_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "opal/http/beast_transport.h"

namespace opal::testing {

// One copy of the ADR-0013 test recorder for the runtime suites and the
// consumer module's tests (the tls_test_identity pattern): a mutex-guarded
// event log plus a bounded wait, because event delivery happens on the
// transport's io thread asynchronously relative to the client's view of
// the close.
struct ConnectionEventRecorder {
  std::mutex mutex;
  std::vector<http::BeastServerTransport::ConnectionEvent> events;

  std::function<void(const http::BeastServerTransport::ConnectionEvent&)> Hook() {
    return [this](const http::BeastServerTransport::ConnectionEvent& event) {
      const std::lock_guard<std::mutex> lock(mutex);
      events.push_back(event);
    };
  }

  // True once at least `count` events arrived within the budget.
  bool WaitFor(std::size_t count, std::chrono::milliseconds budget = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (events.size() >= count) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::lock_guard<std::mutex> lock(mutex);
    return events.size() >= count;
  }
};

}  // namespace opal::testing

#endif  // OPAL_TESTING_CONNECTION_EVENT_RECORDER_H_
