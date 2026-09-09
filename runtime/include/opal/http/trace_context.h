#ifndef OPAL_HTTP_TRACE_CONTEXT_H_
#define OPAL_HTTP_TRACE_CONTEXT_H_

#include <optional>
#include <string>
#include <string_view>

namespace opal::http {

// W3C Trace Context (https://www.w3.org/TR/trace-context/): the traceparent
// header's fields, SDK-free. Backend integration (OpenTelemetry etc.) builds
// on these helpers; docs/production-guide.md has the wiring.
struct TraceContext {
  std::string trace_id;   // 32 lowercase hex digits, never all zeros
  std::string parent_id;  // 16 lowercase hex digits, never all zeros
  bool sampled = true;    // trace-flags bit 0
};

// Parses a traceparent header value. nullopt on malformed input, an
// unsupported all-ones version, or all-zero ids.
std::optional<TraceContext> ParseTraceparent(std::string_view value);

// "00-<trace_id>-<parent_id>-<flags>".
std::string FormatTraceparent(const TraceContext& context);

// A fresh root context: random trace and parent ids, sampled.
TraceContext GenerateTraceContext();

// A fresh random 16-hex-digit span id (for deriving child contexts).
std::string GenerateSpanId();

}  // namespace opal::http

#endif  // OPAL_HTTP_TRACE_CONTEXT_H_
