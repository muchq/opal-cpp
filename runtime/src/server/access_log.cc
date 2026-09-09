#include "opal/server/access_log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "opal/core/fatal.h"
#include "opal/http/forwarded.h"
#include "opal/http/trace_context.h"

namespace opal::server {

namespace {

// The keys. The metric labels where one exists (metrics.cc), so a panel and
// a log query share a vocabulary.
constexpr std::string_view kMethod = "http_method";
constexpr std::string_view kTarget = "target";
constexpr std::string_view kRoute = "route";
constexpr std::string_view kStatus = "status";
constexpr std::string_view kDurationUs = "duration_us";
constexpr std::string_view kRequestBytes = "request_bytes";
constexpr std::string_view kResponseBytes = "response_bytes";
constexpr std::string_view kClient = "client";
constexpr std::string_view kClientSource = "client_source";
constexpr std::string_view kHandlerThrew = "handler_threw";
constexpr std::string_view kTraceId = "trace_id";

// The collision check for `extra`: a built-in cannot be shadowed without
// this list knowing.
constexpr std::array<std::string_view, 11> kBuiltInKeys = {
    kMethod,        kTarget, kRoute,        kStatus,       kDurationUs, kRequestBytes,
    kResponseBytes, kClient, kClientSource, kHandlerThrew, kTraceId};

std::string_view SourceName(http::DerivedClient::Source source) {
  using Source = http::DerivedClient::Source;
  switch (source) {
    case Source::kDirectPeer:
      return "direct_peer";
    case Source::kUntrustedHeaderIgnored:
      return "untrusted_header_ignored";
    case Source::kForwarded:
      return "forwarded";
    case Source::kTrustedTier:
      return "trusted_tier";
    case Source::kUnknown:
      break;
  }
  return "unknown";
}

// Length of the well-formed UTF-8 sequence starting at text[i], or 0 when the
// bytes there are not one (RFC 3629: no overlongs, no surrogates, nothing
// past U+10FFFF, every continuation byte present and in range).
std::size_t Utf8SequenceLength(std::string_view text, std::size_t i) {
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[i + k]); };
  const unsigned char lead = byte(0);
  std::size_t length = 0;
  unsigned char second_min = 0x80;
  unsigned char second_max = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0) second_min = 0xA0;  // overlong
    if (lead == 0xED) second_max = 0x9F;  // surrogates
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    if (lead == 0xF0) second_min = 0x90;  // overlong
    if (lead == 0xF4) second_max = 0x8F;  // past U+10FFFF
  } else {
    return 0;  // 0x80-0xC1 (stray continuation / overlong lead), 0xF5-0xFF
  }
  if (i + length > text.size()) {
    return 0;
  }
  if (byte(1) < second_min || byte(1) > second_max) {
    return 0;
  }
  for (std::size_t k = 2; k < length; ++k) {
    if (byte(k) < 0x80 || byte(k) > 0xBF) {
      return 0;
    }
  }
  return length;
}

void AppendEscaped(std::string& out, std::string_view value) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  out += '"';
  for (std::size_t i = 0; i < value.size();) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c >= 0x80) {
      const std::size_t length = Utf8SequenceLength(value, i);
      if (length == 0) {
        // One replacement per bad byte, so a run of garbage stays the same
        // length in code points as it was in bytes — the reader can see how
        // much was there, just not what.
        out += "\xEF\xBF\xBD";  // U+FFFD
        ++i;
      } else {
        out.append(value, i, length);
        i += length;
      }
      continue;
    }
    ++i;
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0xF];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void AppendKey(std::string& out, std::string_view key) {
  if (out.size() > 1) {
    out += ',';
  }
  AppendEscaped(out, key);
  out += ':';
}

void AppendString(std::string& out, std::string_view key, std::string_view value) {
  AppendKey(out, key);
  AppendEscaped(out, value);
}

template <typename Integer>
void AppendInteger(std::string& out, std::string_view key, Integer value) {
  AppendKey(out, key);
  out += std::to_string(value);
}

// Keys are code constants, and the checks below are what make that
// documentation a contract. Uniqueness has to hold on what reaches the
// object, not on the raw bytes: AppendEscaped replaces every malformed byte
// with U+FFFD, which is not injective, so two distinct malformed keys would
// land as one duplicated key with neither branch below noticing. Rejecting a
// key that is not well-formed UTF-8 keeps raw and emitted equality the same
// thing. An empty key is rejected for the same reason a shadowing one is: it
// is a name nobody can query by.
void CheckExtraKeys(const AccessLogFields& extra) {
  for (std::size_t i = 0; i < extra.size(); ++i) {
    const std::string& key = extra[i].first;
    if (key.empty()) {
      opal::internal::Fatal("opal::server::FormatAccessLog: extra field key is empty");
    }
    for (std::size_t k = 0; k < key.size();) {
      if (static_cast<unsigned char>(key[k]) < 0x80) {
        ++k;
        continue;
      }
      const std::size_t length = Utf8SequenceLength(key, k);
      if (length == 0) {
        opal::internal::Fatal("opal::server::FormatAccessLog: extra field key '" + key +
                              "' is not well-formed UTF-8");
      }
      k += length;
    }
    for (const std::string_view built_in : kBuiltInKeys) {
      if (key == built_in) {
        opal::internal::Fatal("opal::server::FormatAccessLog: extra field '" + key +
                              "' shadows a built-in field");
      }
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (extra[j].first == key) {
        opal::internal::Fatal("opal::server::FormatAccessLog: extra field '" + key +
                              "' is given twice");
      }
    }
  }
}

}  // namespace

std::string FormatAccessLog(const RequestObservation& o, const AccessLogFields& extra) {
  CheckExtraKeys(extra);
  std::string out;
  out.reserve(256);
  out += '{';
  AppendString(out, kMethod, o.method);
  AppendString(out, kTarget, o.target);
  AppendString(out, kRoute, o.operation.empty() ? kUnmatchedRoute : o.operation);
  AppendInteger(out, kStatus, o.status);
  AppendInteger(out, kDurationUs, static_cast<std::int64_t>(o.duration.count()));
  AppendInteger(out, kRequestBytes, static_cast<std::uint64_t>(o.request_bytes));
  AppendInteger(out, kResponseBytes, static_cast<std::uint64_t>(o.response_bytes));
  AppendString(out, kClient, o.client.address);
  AppendString(out, kClientSource, SourceName(o.client.source));
  AppendKey(out, kHandlerThrew);
  out += o.handler_threw ? "true" : "false";
  const auto trace = http::ParseTraceparent(o.trace_parent);
  AppendString(out, kTraceId, trace.has_value() ? trace->trace_id : std::string_view{});
  for (const auto& [key, value] : extra) {
    AppendString(out, key, value);
  }
  out += '}';
  return out;
}

}  // namespace opal::server
