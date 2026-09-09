# Server Middleware (Guard, HealthEndpoint, Observe on_start) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three middleware gaps blocking the portrait pilot — generic admission control (`Guard` + `TooManyRequests`), a static liveness endpoint (`HealthEndpoint`), and an `on_start` callback on `Observe` for in-flight gauges — per `docs/superpowers/specs/2026-07-08-server-middleware-design.md`.

**Architecture:** All three are additions to the existing dependency-free middleware module (`runtime/include/opal/server/middleware.h` + `runtime/src/server/middleware.cc`), composed via the existing `Chain`. No new Bazel targets; no new dependencies (explicitly: no opentelemetry-cpp). One breaking signature change: `Observe` gains `on_start` as its second parameter, so `Observe(cb, now)` call sites become `Observe(cb, nullptr, now)`.

**Tech Stack:** C++20, GoogleTest, Bazel. Repo: `/Users/andy/src/smithy-cpp`, branch `server-middleware` (already checked out).

**Verify environment before Task 1:** `cd /Users/andy/src/smithy-cpp && git branch --show-current` → `server-middleware`.

---

### Task 1: `Guard` middleware + `TooManyRequests` reject factory

**Files:**
- Modify: `runtime/include/opal/server/middleware.h`
- Modify: `runtime/src/server/middleware.cc`
- Test: `runtime/tests/server/middleware_test.cc`

- [ ] **Step 1: Write the failing tests**

Append inside `namespace { ... }` in `runtime/tests/server/middleware_test.cc`, before the closing `}  // namespace` (the file already has helpers `Ok()` and `Tag()` and includes `<chrono>`, `<string>`, `<vector>`):

```cpp
TEST(GuardTest, AdmittedRequestsPassThrough) {
  auto handler = Chain({Guard([](const http::HttpRequest&) { return true; },
                              TooManyRequests())},
                       [](const http::HttpRequest&) { return Ok("served"); });
  const auto response = handler({});
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, "served");
}

TEST(GuardTest, RejectedRequestsShortCircuitWithTheRejectResponse) {
  bool reached = false;
  auto handler = Chain({Guard([](const http::HttpRequest&) { return false; },
                              [](const http::HttpRequest&) {
                                http::HttpResponse response;
                                response.status = 503;
                                response.body = "maintenance";
                                return response;
                              })},
                       [&](const http::HttpRequest&) {
                         reached = true;
                         return Ok("never");
                       });
  const auto response = handler({});
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(response.body, "maintenance");
  EXPECT_FALSE(reached);
}

TEST(GuardTest, AdmitSeesTheRequest) {
  // The rate-limiting instantiation: admit keys on a header.
  auto handler = Chain(
      {Guard(
           [](const http::HttpRequest& request) {
             return request.headers.Get("x-forwarded-for").value_or("") != "10.0.0.1";
           },
           TooManyRequests())},
      [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest allowed;
  allowed.headers.Set("x-forwarded-for", "10.0.0.2");
  EXPECT_EQ(handler(allowed).status, 200);

  http::HttpRequest limited;
  limited.headers.Set("x-forwarded-for", "10.0.0.1");
  EXPECT_EQ(handler(limited).status, 429);
}

TEST(TooManyRequestsTest, ShapesThe429) {
  const auto response = TooManyRequests()(http::HttpRequest{});
  EXPECT_EQ(response.status, 429);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
  EXPECT_EQ(response.body, R"({"error":"Too many requests"})");
  EXPECT_FALSE(response.headers.Has("retry-after"));
}

TEST(TooManyRequestsTest, SetsRetryAfterWhenGiven) {
  const auto response = TooManyRequests(std::chrono::seconds(30))(http::HttpRequest{});
  EXPECT_EQ(response.status, 429);
  EXPECT_EQ(response.headers.Get("retry-after").value_or(""), "30");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime:middleware_test --test_output=errors
```

Expected: FAIL to compile — `error: use of undeclared identifier 'Guard'` (and `TooManyRequests`).

- [ ] **Step 3: Declare in the header**

In `runtime/include/opal/server/middleware.h`: add `#include <optional>` to the includes (keep the include list sorted: `<chrono>`, `<functional>`, `<optional>`, `<string>`, `<vector>`), then add after the `Chain` declaration (line 25, before `RequestObservation`):

```cpp
// Admission control outside the router: admit(request) true passes the
// request through, false short-circuits with reject(request). The policy —
// a rate limiter, an IP allowlist, a maintenance switch — is the
// application's dependency; Guard owns only the composition point and the
// short-circuit. Neither callback may be null.
Middleware Guard(std::function<bool(const http::HttpRequest&)> admit,
                 std::function<http::HttpResponse(const http::HttpRequest&)> reject);

// Ready-made Guard rejection for rate limiting: 429 with body
// {"error":"Too many requests"}, plus a Retry-After header (seconds) when
// retry_after is set.
std::function<http::HttpResponse(const http::HttpRequest&)> TooManyRequests(
    std::optional<std::chrono::seconds> retry_after = std::nullopt);
```

- [ ] **Step 4: Implement in middleware.cc**

Add to `runtime/src/server/middleware.cc` after `Chain` (before `Observe`). The file already includes `<optional>`, `<string>`, `<utility>`:

```cpp
Middleware Guard(std::function<bool(const http::HttpRequest&)> admit,
                 std::function<http::HttpResponse(const http::HttpRequest&)> reject) {
  return [admit = std::move(admit), reject = std::move(reject)](http::RequestHandler next) {
    return [admit, reject, next = std::move(next)](const http::HttpRequest& request) {
      if (!admit(request)) {
        return reject(request);
      }
      return next(request);
    };
  };
}

std::function<http::HttpResponse(const http::HttpRequest&)> TooManyRequests(
    std::optional<std::chrono::seconds> retry_after) {
  return [retry_after](const http::HttpRequest&) {
    http::HttpResponse response;
    response.status = 429;
    response.headers.Set("content-type", "application/json");
    if (retry_after.has_value()) {
      response.headers.Set("retry-after", std::to_string(retry_after->count()));
    }
    response.body = R"({"error":"Too many requests"})";
    return response;
  };
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime:middleware_test --test_output=errors
```

Expected: PASS (all existing tests plus the 5 new ones).

- [ ] **Step 6: Commit**

```bash
cd /Users/andy/src/smithy-cpp && git add runtime/include/opal/server/middleware.h runtime/src/server/middleware.cc runtime/tests/server/middleware_test.cc && git commit -m "feat(runtime): Guard admission middleware with TooManyRequests factory"
```

---

### Task 2: `HealthEndpoint` middleware

**Files:**
- Modify: `runtime/include/opal/server/middleware.h`
- Modify: `runtime/src/server/middleware.cc`
- Test: `runtime/tests/server/middleware_test.cc`

- [ ] **Step 1: Write the failing tests**

Append inside the anonymous namespace in `runtime/tests/server/middleware_test.cc`:

```cpp
TEST(HealthEndpointTest, AnswersGetOnThePath) {
  bool reached = false;
  auto handler = Chain({HealthEndpoint()}, [&](const http::HttpRequest&) {
    reached = true;
    return Ok("router");
  });

  http::HttpRequest request;
  request.method = "GET";
  request.target = "/health";
  const auto response = handler(request);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
  EXPECT_EQ(response.body, R"({"status":"healthy"})");
  EXPECT_FALSE(reached);
}

TEST(HealthEndpointTest, IgnoresTheQueryString) {
  auto handler = Chain({HealthEndpoint()}, [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/health?verbose=1";
  EXPECT_EQ(handler(request).body, R"({"status":"healthy"})");
}

TEST(HealthEndpointTest, PassesThroughOtherMethodsAndTargets) {
  auto handler = Chain({HealthEndpoint()}, [](const http::HttpRequest&) { return Ok("router"); });

  http::HttpRequest post;
  post.method = "POST";
  post.target = "/health";
  EXPECT_EQ(handler(post).body, "router");  // the router decides (404/405/route)

  http::HttpRequest other;
  other.method = "GET";
  other.target = "/healthz";
  EXPECT_EQ(handler(other).body, "router");
}

TEST(HealthEndpointTest, CustomPath) {
  auto handler =
      Chain({HealthEndpoint("/status/live")}, [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/status/live";
  EXPECT_EQ(handler(request).status, 200);

  request.target = "/health";
  EXPECT_EQ(handler(request).body, "router");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime:middleware_test --test_output=errors
```

Expected: FAIL to compile — `error: use of undeclared identifier 'HealthEndpoint'`.

- [ ] **Step 3: Declare in the header**

In `runtime/include/opal/server/middleware.h`, after the `TooManyRequests` declaration:

```cpp
// Static liveness endpoint: answers GET <path> (query string ignored) with
// 200 {"status":"healthy"}; every other request passes through to the next
// handler, so a model may still define other routes on the path. Readiness
// probing is deliberately out of scope.
Middleware HealthEndpoint(std::string path = "/health");
```

- [ ] **Step 4: Implement in middleware.cc**

Add `#include <string_view>` to the includes of `runtime/src/server/middleware.cc`, then after `TooManyRequests`:

```cpp
Middleware HealthEndpoint(std::string path) {
  return [path = std::move(path)](http::RequestHandler next) {
    return [path, next = std::move(next)](const http::HttpRequest& request) {
      const std::string_view target(request.target);
      if (request.method == "GET" && target.substr(0, target.find('?')) == path) {
        http::HttpResponse response;
        response.status = 200;
        response.headers.Set("content-type", "application/json");
        response.body = R"({"status":"healthy"})";
        return response;
      }
      return next(request);
    };
  };
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime:middleware_test --test_output=errors
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
cd /Users/andy/src/smithy-cpp && git add runtime/include/opal/server/middleware.h runtime/src/server/middleware.cc runtime/tests/server/middleware_test.cc && git commit -m "feat(runtime): HealthEndpoint liveness middleware"
```

---

### Task 3: `Observe` gains `on_start` and pairs completions when dispatch throws

**Files:**
- Modify: `runtime/include/opal/server/middleware.h`
- Modify: `runtime/src/server/middleware.cc`
- Test: `runtime/tests/server/middleware_test.cc` (new tests + one existing call site updated)

- [ ] **Step 1: Write the failing tests**

Append inside the anonymous namespace in `runtime/tests/server/middleware_test.cc`:

```cpp
TEST(ObserveTest, OnStartFiresBeforeDispatch) {
  std::vector<std::string> log;
  auto handler = Chain({Observe([&](const RequestObservation&) { log.push_back("complete"); },
                                [&](const RequestStart& s) {
                                  log.push_back("start:" + s.method + " " + s.target);
                                })},
                       [&](const http::HttpRequest&) {
                         log.push_back("handler");
                         return Ok("done");
                       });

  http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  (void)handler(request);
  EXPECT_EQ(log, (std::vector<std::string>{"start:POST /tasks", "handler", "complete"}));
}

TEST(ObserveTest, PairsCompleteWithStartWhenDispatchThrows) {
  int started = 0;
  std::vector<RequestObservation> completions;
  auto handler = Chain({Observe([&](const RequestObservation& o) { completions.push_back(o); },
                                [&](const RequestStart&) { ++started; })},
                       [](const http::HttpRequest&) -> http::HttpResponse {
                         throw std::runtime_error("handler exploded");
                       });

  EXPECT_THROW((void)handler({}), std::runtime_error);  // containment stays upstream
  EXPECT_EQ(started, 1);
  ASSERT_EQ(completions.size(), 1u);  // an in-flight gauge never leaks
  EXPECT_EQ(completions[0].status, 500);
  EXPECT_EQ(completions[0].operation, "");
}

TEST(ObserveTest, ThrowingOnStartIsContainedAndTheRequestIsServed) {
  auto handler = Chain({Observe([](const RequestObservation&) {},
                                [](const RequestStart&) {
                                  throw std::runtime_error("gauge backend down");
                                })},
                       [](const http::HttpRequest&) { return Ok("served"); });
  http::HttpResponse response;
  EXPECT_NO_THROW(response = handler({}));
  EXPECT_EQ(response.body, "served");
}
```

Then update the ONE existing call site that passes the injectable clock as the second argument — `ObserveTest.ReportsMethodTargetStatusAndDuration` (currently `middleware_test.cc:79`):

```cpp
  // was: Chain({Observe([&](const RequestObservation& o) { observations.push_back(o); }, now)}, ...
  auto handler =
      Chain({Observe([&](const RequestObservation& o) { observations.push_back(o); }, nullptr,
                     now)},
            [](const http::HttpRequest&) {
              http::HttpResponse response;
              response.status = 404;
              return response;
            });
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime:middleware_test --test_output=errors
```

Expected: FAIL to compile — `RequestStart` undeclared, and no 3-argument `Observe` overload.

- [ ] **Step 3: Update the header**

In `runtime/include/opal/server/middleware.h`, add before the `Observe` declaration:

```cpp
// What on_start sees, before the router runs. The Smithy operation is not
// yet known pre-dispatch, so start observations are labeled by method and
// target only.
struct RequestStart {
  std::string method;
  std::string target;
};
```

Replace the existing `Observe` declaration and its doc comment (currently lines 42-48) with:

```cpp
// Middleware reporting every request to callbacks — the structured-logging
// and metrics hook. on_complete runs after the response is built (count =
// callbacks, latency = duration); the optional on_start runs before dispatch
// so an in-flight gauge can increment (pair it with on_complete's decrement —
// the two always pair, even when dispatch throws: the completion then
// reports status 500 with an empty operation before the exception continues
// to the transport's containment). Throwing callbacks are logged and
// swallowed. Callbacks run on the transport's request thread; keep them
// cheap or hand off. now is injectable for deterministic tests (null means
// steady_clock).
Middleware Observe(std::function<void(const RequestObservation&)> on_complete,
                   std::function<void(const RequestStart&)> on_start = nullptr,
                   std::function<std::chrono::steady_clock::time_point()> now = nullptr);
```

- [ ] **Step 4: Reimplement Observe in middleware.cc**

Replace the whole existing `Observe` function in `runtime/src/server/middleware.cc` (currently lines 22-51) with the following. It factors the existing throwing-sink containment into a helper placed in a new anonymous namespace directly above `Observe`:

```cpp
namespace {

// A throwing observation sink (e.g. a metrics backend under backpressure)
// must not discard a response or unwind into the transport thread; swallow
// it after logging.
template <typename Callback, typename Observation>
void CallContained(const Callback& callback, const Observation& observation, const char* which) {
  try {
    callback(observation);
  } catch (const std::exception& e) {
    std::clog << "opal: " << which << " callback threw: " << e.what() << "\n";
  } catch (...) {
    std::clog << "opal: " << which << " callback threw a non-std exception\n";
  }
}

}  // namespace

Middleware Observe(std::function<void(const RequestObservation&)> on_complete,
                   std::function<void(const RequestStart&)> on_start,
                   std::function<std::chrono::steady_clock::time_point()> now) {
  if (now == nullptr) {
    now = [] { return std::chrono::steady_clock::now(); };
  }
  return [on_complete = std::move(on_complete), on_start = std::move(on_start),
          now = std::move(now)](http::RequestHandler next) {
    return [on_complete, on_start, now,
            next = std::move(next)](const http::HttpRequest& request) {
      if (on_start != nullptr) {
        CallContained(on_start, RequestStart{request.method, request.target}, "Observe on_start");
      }
      RequestObservation observation;
      observation.method = request.method;
      observation.target = request.target;
      observation.trace_parent = request.headers.Get("traceparent").value_or("");
      const auto start = now();
      http::HttpResponse response;
      try {
        response = next(request);
      } catch (...) {
        // Keep start/complete paired when dispatch throws: report a 500
        // completion, then let the exception continue to the transport's
        // containment (server_dispatch.h).
        observation.status = 500;
        observation.duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
        CallContained(on_complete, observation, "Observe on_complete");
        throw;
      }
      observation.operation = response.operation;
      observation.status = response.status;
      observation.duration = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
      CallContained(on_complete, observation, "Observe on_complete");
      return response;
    };
  };
}
```

Note: `middleware.cc` already includes `<exception>` and `<iostream>`; no include changes.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime:middleware_test --test_output=errors
```

Expected: PASS — including the pre-existing `ObserveTest` cases (`CountsEveryRequest`, `ThrowingCallbackDoesNotDiscardResponseOrPropagate`), which compile unchanged against the new defaulted parameters.

- [ ] **Step 6: Check for other Observe call sites in the repo**

```bash
cd /Users/andy/src/smithy-cpp && grep -rn "Observe(" --include='*.cc' --include='*.h' runtime/ examples/ benchmarks/ fuzz/ | grep -v ObserveAttempts | grep -v middleware
```

Expected: no output (the only in-repo server-side `Observe` call sites are middleware_test.cc, already updated). If anything appears, update it to the new signature the same way (`Observe(cb, nullptr, now)`).

- [ ] **Step 7: Run the full runtime test suite**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //runtime/... --test_output=errors
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
cd /Users/andy/src/smithy-cpp && git add runtime/include/opal/server/middleware.h runtime/src/server/middleware.cc runtime/tests/server/middleware_test.cc && git commit -m "feat(runtime): Observe on_start callback with paired completions on throw"
```

---

### Task 4: Compose all three in the bazel-consumer integration test

**Files:**
- Modify: `examples/bazel-consumer/todo_integration_test.cc`
- Modify: `examples/bazel-consumer/BUILD.bazel`

- [ ] **Step 1: Write the failing test**

In `examples/bazel-consumer/todo_integration_test.cc`, make the include block (currently lines 5-19) read:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "acme/todo/cbor/client.h"
#include "acme/todo/cbor/server.h"
#include "acme/todo/client.h"
#include "acme/todo/jsonrpc/client.h"
#include "acme/todo/jsonrpc/server.h"
#include "acme/todo/server.h"
#include "opal/client/config.h"
#include "opal/http/loopback.h"
#include "opal/http/socket_transport.h"
#include "opal/server/middleware.h"
```

Then append this test at the end of the anonymous namespace (after `TodoJsonRpcTest`, before the closing `}  // namespace`):

```cpp
// The production middleware chain from the guide — Guard, then Observe, then
// HealthEndpoint — composed around a generated server and driven by the
// generated client.
TEST(TodoMiddlewareTest, GuardObserveAndHealthComposeAroundTheServer) {
  TodoServer server(std::make_shared<InMemoryHandler>());
  int started = 0;
  int completed = 0;
  bool admit = true;
  auto handler = opal::server::Chain(
      {opal::server::Guard([&admit](const opal::http::HttpRequest&) { return admit; },
                             opal::server::TooManyRequests(std::chrono::seconds(1))),
       opal::server::Observe(
           [&completed](const opal::server::RequestObservation&) { ++completed; },
           [&started](const opal::server::RequestStart&) { ++started; }),
       opal::server::HealthEndpoint()},
      server.Handler());

  auto loopback = std::make_shared<opal::http::Loopback>();
  ASSERT_TRUE(loopback->Start(handler).ok());

  // Liveness answers without reaching the router, and is observed.
  opal::http::HttpRequest health;
  health.method = "GET";
  health.target = "/health";
  const auto health_response = loopback->Send(health);
  ASSERT_TRUE(health_response.ok());
  EXPECT_EQ(health_response->status, 200);
  EXPECT_EQ(health_response->body, R"({"status":"healthy"})");

  // The generated client works through the chain.
  opal::ClientConfig config;
  config.http_client = loopback;
  auto created = TodoClient::Create(std::move(config));
  ASSERT_TRUE(created.ok()) << created.error().message();
  TodoClient client = std::move(*created);
  const auto added = client.AddTask(AddTaskInput{.title = "compose middleware"});
  ASSERT_TRUE(added.ok()) << added.error().message();

  // Once admit flips, Guard sheds load with the shaped 429 before Observe.
  admit = false;
  opal::http::HttpRequest denied;
  denied.method = "POST";
  denied.target = "/tasks";
  const auto denied_response = loopback->Send(denied);
  ASSERT_TRUE(denied_response.ok());
  EXPECT_EQ(denied_response->status, 429);
  EXPECT_EQ(denied_response->headers.Get("retry-after").value_or(""), "1");

  EXPECT_EQ(started, 2);  // health + AddTask; the rejected request never reached Observe
  EXPECT_EQ(completed, 2);
}
```

In `examples/bazel-consumer/BUILD.bazel`, add `"@smithy_cpp//runtime:server",` to the `todo_integration_test` deps (keep the list sorted):

```starlark
    deps = [
        ":todo_cbor_client",
        ":todo_cbor_server",
        ":todo_client",
        ":todo_jsonrpc_client",
        ":todo_jsonrpc_server",
        ":todo_server",
        "@googletest//:gtest_main",
        "@smithy_cpp//runtime:client",
        "@smithy_cpp//runtime:http",
        "@smithy_cpp//runtime:server",
    ],
```

- [ ] **Step 2: Run the consumer test**

The consumer example is its own Bazel module consuming smithy-cpp by `local_path_override`/`git_override`; run it from its directory:

```bash
cd /Users/andy/src/smithy-cpp/examples/bazel-consumer && bazel test //:todo_integration_test --test_output=errors
```

Expected: PASS. (If Tasks 1-3 were skipped or incomplete this fails to compile — that ordering is the point; there is no separate red step for an integration test whose ingredients already exist.)

- [ ] **Step 3: Commit**

```bash
cd /Users/andy/src/smithy-cpp && git add examples/bazel-consumer/todo_integration_test.cc examples/bazel-consumer/BUILD.bazel && git commit -m "test(examples): compose Guard, Observe, and HealthEndpoint in the consumer integration test"
```

---

### Task 5: Documentation — production-guide and CHANGELOG

**Files:**
- Modify: `docs/production-guide.md` (the "Server middleware" section, currently starting line 168)
- Modify: `CHANGELOG.md` (the `### Runtime` list under `## [Unreleased]`, currently starting line 44)

- [ ] **Step 1: Extend the "Server middleware" section**

In `docs/production-guide.md`, replace the existing composition example (the `WeatherServer server(handler);` block through the paragraph ending "keep it cheap or hand off.", currently lines 175-196) with:

````markdown
```cpp
WeatherServer server(handler);

// Policy stays an application dependency (your rate limiter, your metrics
// backend); the middleware owns only the composition point.
auto limiter = std::make_shared<MyRateLimiter>(/* window, budget */);

transport.Start(opal::server::Chain(
    {// Outermost: shed abusive traffic before it costs anything.
     opal::server::Guard(
         [limiter](const opal::http::HttpRequest& request) {
           return limiter->Allow(
               request.headers.Get("x-forwarded-for").value_or(""));
         },
         opal::server::TooManyRequests(std::chrono::seconds(30))),
     // Observe everything admitted — health probes included. on_start
     // (optional) enables an in-flight gauge; on_complete carries
     // method/target/operation/status/duration/trace_parent.
     opal::server::Observe(
         [](const opal::server::RequestObservation& o) {
           // gauge -1; count 1; latency o.duration — feed any backend.
         },
         [](const opal::server::RequestStart& s) {
           // gauge +1 (labeled by s.method/s.target; the operation is not
           // known until the router runs).
         }),
     // Liveness: GET /health -> 200 {"status":"healthy"}; everything else
     // passes through to the router.
     opal::server::HealthEndpoint()},
    server.Handler()));
```

The first middleware in the chain is outermost: it sees the request first and
can short-circuit before anything below it runs. `Guard` is the generic
admission primitive — rate limiting (above), IP allowlists, maintenance mode —
admit/reject callbacks in, one decision point out. `Observe`'s callbacks run
on the transport's request thread (keep them cheap or hand off) and always
pair: when dispatch throws, `on_complete` reports a 500 completion before the
exception reaches the transport's containment, so an in-flight gauge can never
leak. Throwing callbacks are logged and swallowed.
````

Keep the section's opening paragraph ("Generated servers expose their router...") unchanged, and delete the old `require_auth` lambda example — the auth story is already covered by the `RequireBearerAuth`/`RequireApiKeyHeader` paragraph earlier in the guide (line ~105), and the new block replaces the composition example wholesale.

- [ ] **Step 2: Update the Observability section's server paragraph**

In the same file's "Observability" section, the paragraph beginning `**Server:** ``Observe`` (above) reports, per request:` — append one sentence:

```markdown
An optional `on_start` callback fires before dispatch (method and target
only), enabling in-flight gauges; start/complete always pair, even when the
handler throws.
```

- [ ] **Step 3: Add the CHANGELOG entry**

In `CHANGELOG.md`, append this bullet to the `### Runtime` list (after the existing "Transports:" bullet):

```markdown
- Server middleware additions for production serving: `Guard` admission
  control (rate limiting, allowlists, maintenance mode — policy stays an
  application dependency) with a `TooManyRequests` reject factory,
  `HealthEndpoint` static liveness, and an optional `Observe` `on_start`
  callback for in-flight gauges with guaranteed start/complete pairing.
  **Breaking:** `Observe(callback, now)` call sites become
  `Observe(callback, nullptr, now)`.
```

- [ ] **Step 4: Verify the docs build nothing is broken (docs are plain markdown — just sanity-scan)**

```bash
cd /Users/andy/src/smithy-cpp && grep -n "Guard\|HealthEndpoint\|on_start" docs/production-guide.md CHANGELOG.md | head -20
```

Expected: hits in both files at the locations edited above.

- [ ] **Step 5: Commit**

```bash
cd /Users/andy/src/smithy-cpp && git add docs/production-guide.md CHANGELOG.md && git commit -m "docs: Guard, HealthEndpoint, and Observe on_start in the production guide"
```

---

### Task 6: Full verification

- [ ] **Step 1: Run the whole repo's tests**

```bash
cd /Users/andy/src/smithy-cpp && bazel test //... --test_output=errors
```

Expected: PASS (protocol conformance, codegen goldens, runtime, fuzz smoke — everything).

- [ ] **Step 2: Run the consumer module's tests**

```bash
cd /Users/andy/src/smithy-cpp/examples/bazel-consumer && bazel test //... --test_output=errors
```

Expected: PASS.

- [ ] **Step 3: Review the branch diff**

```bash
cd /Users/andy/src/smithy-cpp && git log --oneline main..HEAD && git diff main --stat
```

Expected: the spec/plan docs plus five implementation commits touching exactly: `runtime/include/opal/server/middleware.h`, `runtime/src/server/middleware.cc`, `runtime/tests/server/middleware_test.cc`, `examples/bazel-consumer/todo_integration_test.cc`, `examples/bazel-consumer/BUILD.bazel`, `docs/production-guide.md`, `CHANGELOG.md`.
