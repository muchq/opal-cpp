#include "smithy/http/trace_context.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string_view>

namespace opal::http {
namespace {

bool IsLowerHex(std::string_view text) {
  return std::ranges::all_of(
      text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool AllZeros(std::string_view text) {
  return text.find_first_not_of('0') == std::string_view::npos;
}

std::string RandomHex(int digits) {
  thread_local std::mt19937_64 engine{std::random_device{}()};
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<std::size_t>(digits));
  std::uint64_t bits = 0;
  for (int i = 0; i < digits; ++i) {
    if (i % 16 == 0) bits = engine();
    out.push_back(kHex[bits & 0xF]);
    bits >>= 4;
  }
  return out;
}

}  // namespace

std::optional<TraceContext> ParseTraceparent(std::string_view value) {
  // version(2) '-' trace-id(32) '-' parent-id(16) '-' flags(2); longer
  // values are allowed for future versions if a '-' follows.
  if (value.size() < 55 || value[2] != '-' || value[35] != '-' || value[52] != '-') {
    return std::nullopt;
  }
  if (value.size() > 55 && value[55] != '-') {
    return std::nullopt;
  }
  const std::string_view version = value.substr(0, 2);
  const std::string_view trace_id = value.substr(3, 32);
  const std::string_view parent_id = value.substr(36, 16);
  const std::string_view flags = value.substr(53, 2);
  if (!IsLowerHex(version) || version == "ff" || !IsLowerHex(trace_id) || !IsLowerHex(parent_id) ||
      !IsLowerHex(flags)) {
    return std::nullopt;
  }
  if (AllZeros(trace_id) || AllZeros(parent_id)) {
    return std::nullopt;
  }
  TraceContext context;
  context.trace_id = std::string(trace_id);
  context.parent_id = std::string(parent_id);
  const char low = flags[1];
  const int low_value = low <= '9' ? low - '0' : low - 'a' + 10;
  context.sampled = (low_value & 1) != 0;
  return context;
}

std::string FormatTraceparent(const TraceContext& context) {
  return "00-" + context.trace_id + "-" + context.parent_id + (context.sampled ? "-01" : "-00");
}

TraceContext GenerateTraceContext() {
  TraceContext context;
  do {
    context.trace_id = RandomHex(32);
  } while (AllZeros(context.trace_id));
  context.parent_id = GenerateSpanId();
  context.sampled = true;
  return context;
}

std::string GenerateSpanId() {
  std::string id;
  do {
    id = RandomHex(16);
  } while (AllZeros(id));
  return id;
}

}  // namespace opal::http
