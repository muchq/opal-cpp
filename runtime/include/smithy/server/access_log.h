#ifndef SMITHY_SERVER_ACCESS_LOG_H_
#define SMITHY_SERVER_ACCESS_LOG_H_

#include <string>
#include <utility>
#include <vector>

#include "smithy/server/middleware.h"

namespace smithy::server {

// The structured access log (issue #203): one RequestObservation in, one
// line of JSON out. A pure function — no I/O, no sink, no configuration, no
// new dependency. Where the string goes is the consumer's decision:
//
//   Observe([](const RequestObservation& o) {
//            std::clog << FormatAccessLog(o, {{"service_name", "todo-service"}}) << '\n';
//          },
//          nullptr, nullptr, trusted);
//
// Why this is in the runtime rather than in each consumer: the field names
// are the metric labels, so a spike on a `route="AddTask"` panel pastes into
// a log query and means the same thing; and `target` is attacker-controlled,
// so the escaping that keeps a crafted URI from terminating the record early
// and masquerading as its own log entry is written once, with a test that
// feeds it a control byte, rather than re-derived per service.
//
// The line, in this key order, every key always present:
//
//   http_method     the wire method, verbatim. Not collapsed to the metrics'
//                   CUSTOM sentinel: that collapse defends series cardinality,
//                   which a log line has no reason to fear — and the log is
//                   where you find out what the invented verb actually was.
//   target          the request target, verbatim (escaped). Never a metric
//                   label; this is where the path parameters live.
//   route           RequestObservation::operation, or the metrics' `unmatched`
//                   sentinel when it is empty (a 404/405/400 dispatch failure,
//                   or a Guard rejection that never reached the router) — the
//                   same spelling the scrape uses, so the pivot holds both
//                   ways and an empty string never reads as "no route field".
//   status          integer.
//   duration_us     integer microseconds — the histogram's unit, so a reader
//                   can compare a line against a bucket without converting.
//   request_bytes, response_bytes
//                   integers.
//   client          RequestObservation::client.address: the ADR-0012 derived
//                   client, never the raw x-forwarded-for.
//   client_source   its provenance, as one of "direct_peer",
//                   "untrusted_header_ignored", "forwarded", "trusted_tier",
//                   "unknown". The distribution is the misconfiguration signal
//                   docs/production-guide.md reads.
//   handler_threw   boolean.
//   trace_id        the 32-hex trace id parsed from trace_parent, or "" when
//                   the header is absent or malformed (only a chain driven
//                   directly in tests; the transport mints one, ADR-0011).
//   <extra...>      the caller's fields, in the order given.
//
// There is deliberately no timestamp: the observation carries none, and every
// sink that would receive this line stamps its own arrival time; two
// timestamps on one record is one more than anyone can reconcile.
//
// Every string value is JSON-escaped: quote, backslash, and every control
// character below 0x20 (short forms for \b \f \n \r \t, \uXXXX otherwise).
// Bytes at or above 0x80 pass through as UTF-8; a byte sequence that is not
// valid UTF-8 is replaced by U+FFFD (as `�`) rather than passed through,
// because a strict parser rejects a record containing one, and an access log
// that a collector drops on the request that was malformed is a log that
// goes quiet exactly when it is needed.
//
// `extra` is how a service attaches what the observation cannot know — its
// own `service_name` (the label the metrics contract requires, so a line can
// pivot to a panel), a tenant, a request id. Keys are code constants, never
// request data, so a key that collides with a built-in, repeats within
// `extra`, is empty, or is not well-formed UTF-8 aborts (ADR-0009): duplicate
// keys in JSON are ambiguous, and a collector that resolves them silently
// would put the wrong value under the right name. The UTF-8 rule is what
// keeps the uniqueness check honest — a malformed key would be replaced on
// the way out, and two distinct malformed keys can replace to one. Values
// are data and are escaped like every other string.
using AccessLogFields = std::vector<std::pair<std::string, std::string>>;

std::string FormatAccessLog(const RequestObservation& observation,
                            const AccessLogFields& extra = {});

}  // namespace smithy::server

#endif  // SMITHY_SERVER_ACCESS_LOG_H_
