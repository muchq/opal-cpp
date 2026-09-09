// Fuzz target: the access-log formatter (issue #203). `target` and `method`
// reach the line straight off the wire, so the invariant under attack is
// log injection: whatever bytes go in, the output must be exactly one JSON
// object that a strict parser accepts, with the documented key set and
// nothing more, and every string must read back as what went in (bad UTF-8
// excepted — it is replaced, never passed through, so the parser accepts
// the line rather than dropping the one record about the malformed request).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "nlohmann/json.hpp"
#include "smithy/server/access_log.h"
#include "smithy/server/middleware.h"

namespace {

// Whether `text` is well-formed UTF-8 — the precondition under which the
// formatter promises to pass a string through unchanged. Judged by the
// parser library rather than by a second copy of the formatter's own
// validator, so a shared misreading of RFC 3629 cannot hide itself.
bool IsValidUtf8(const std::string& text) {
  try {
    (void)nlohmann::json(text).dump();
    return true;
  } catch (const nlohmann::json::type_error&) {
    return false;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Layout: method '\n' target '\n' operation '\n' extra-value '\n' traceparent.
  const std::string all(reinterpret_cast<const char*>(data), size);
  smithy::server::RequestObservation o;
  std::string extra_value;
  std::string* fields[] = {&o.method, &o.target, &o.operation, &extra_value, &o.trace_parent};
  std::size_t start = 0;
  for (std::string* field : fields) {
    const auto newline = all.find('\n', start);
    *field = all.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  o.status = size == 0 ? 0 : 100 + data[0] % 500;
  o.client.address = "203.0.113.7";
  o.client.source = smithy::http::DerivedClient::Source::kForwarded;

  const std::string line =
      smithy::server::FormatAccessLog(o, {{"service_name", "svc"}, {"tenant", extra_value}});

  // One object, strictly parsed, no trailing bytes. accept() is the strict
  // parser a collector would be: it rejects unescaped control characters and
  // invalid UTF-8 alike.
  if (!nlohmann::json::accept(line)) std::abort();
  const nlohmann::json parsed = nlohmann::json::parse(line);
  if (!parsed.is_object() || parsed.size() != 13) std::abort();

  // A string that was valid UTF-8 comes back byte-identical: escaping is
  // reversible, and nothing that was not a control character or JSON syntax
  // was touched.
  const auto roundtrips = [&](const char* key, const std::string& original) {
    if (IsValidUtf8(original) && parsed.at(key).get<std::string>() != original) std::abort();
  };
  roundtrips("http_method", o.method);
  roundtrips("target", o.target);
  roundtrips("tenant", extra_value);
  if (!o.operation.empty()) roundtrips("route", o.operation);
  if (parsed.at("status").get<int>() != o.status) std::abort();
  if (parsed.at("service_name").get<std::string>() != "svc") std::abort();
  return 0;
}
