#ifndef OPAL_TESTING_PROTOCOL_TEST_H_
#define OPAL_TESTING_PROTOCOL_TEST_H_

// Support library for the generated Smithy protocol conformance tests
// (smithy.test#httpRequestTests / #httpResponseTests). Test-only; ships in
// the //runtime:protocol_test_support target, never in production code.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "opal/cbor/cbor.h"
#include "opal/core/base64.h"
#include "opal/core/document.h"
#include "opal/core/outcome.h"
#include "opal/http/message.h"
#include "opal/http/transport.h"
#include "opal/http/uri.h"
#include "opal/json/json.h"

namespace opal::testing {

// Records the request and answers with a canned response, so generated tests
// can assert the exact wire shape a client produced or feed it a response.
class CapturingTransport final : public opal::http::HttpClient {
 public:
  opal::Outcome<opal::http::HttpResponse> Send(const opal::http::HttpRequest& request) override {
    last_request = request;
    return next_response;
  }

  opal::http::HttpRequest last_request;
  opal::http::HttpResponse next_response{200, {}, ""};
};

// The rpcv2Cbor request preamble every hand-written server-side wire test
// builds: POST /service/<Service>/operation/<Operation> plus the protocol
// and content-type headers. One home instead of a copy per test file — the
// generated conformance tests regenerate their own copies, these don't.
inline opal::http::HttpRequest Rpcv2CborRequest(const std::string& service,
                                                const std::string& operation,
                                                std::string body = "") {
  opal::http::HttpRequest request;
  request.method = "POST";
  request.target = "/service/" + service + "/operation/" + operation;
  request.headers.Set("smithy-protocol", "rpc-v2-cbor");
  request.headers.Set("content-type", "application/cbor");
  request.body = std::move(body);
  return request;
}

// Wraps a transport and mutates successful responses in flight — e.g. the
// integration tests inject unknown body members that clients must ignore.
class MutatingTransport final : public opal::http::HttpClient {
 public:
  using Mutator = std::function<void(opal::http::HttpResponse&)>;

  MutatingTransport(std::shared_ptr<opal::http::HttpClient> inner, Mutator mutate)
      : inner_(std::move(inner)), mutate_(std::move(mutate)) {}

  opal::Outcome<opal::http::HttpResponse> Send(const opal::http::HttpRequest& request) override {
    auto response = inner_->Send(request);
    if (response.ok()) mutate_(*response);
    return response;
  }

 private:
  std::shared_ptr<opal::http::HttpClient> inner_;
  Mutator mutate_;
};

// Path portion of an origin-form request target ("/a/b?c=d" -> "/a/b").
inline std::string UriPath(const std::string& target) { return target.substr(0, target.find('?')); }

// Raw (still percent-encoded) query entries, e.g. {"a=b", "flag"}.
inline std::vector<std::string> QueryEntries(const std::string& target) {
  std::vector<std::string> entries;
  const auto question = target.find('?');
  if (question == std::string::npos) return entries;
  std::string_view query = std::string_view(target).substr(question + 1);
  while (!query.empty()) {
    const auto amp = query.find('&');
    entries.emplace_back(query.substr(0, amp));
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return entries;
}

inline std::string QueryKey(std::string_view entry) {
  return std::string(entry.substr(0, entry.find('=')));
}

// Percent-decodes an entry so comparisons ignore encoding differences: a test
// definition may write "query=the query" while the wire (correctly) carries
// "query=the%20query". Falls back to the raw text if the escapes are malformed.
inline std::string DecodeQueryEntry(const std::string& entry) {
  auto decoded = opal::http::PercentDecode(entry);
  return decoded.ok() ? *decoded : entry;
}

// Every expected entry must appear in the target's query string, counting
// duplicates (multiset containment); extra actual entries are allowed.
// Comparison is done on percent-decoded entries so encoding is not significant.
inline ::testing::AssertionResult QueryContains(const std::string& target,
                                                const std::vector<std::string>& expected) {
  std::vector<std::string> actual;
  for (const std::string& entry : QueryEntries(target)) {
    actual.push_back(DecodeQueryEntry(entry));
  }
  for (const std::string& raw : expected) {
    const std::string entry = DecodeQueryEntry(raw);
    const auto it = std::find(actual.begin(), actual.end(), entry);
    if (it == actual.end()) {
      return ::testing::AssertionFailure()
             << "query entry \"" << entry << "\" not found in target \"" << target << "\"";
    }
    actual.erase(it);
  }
  return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult QueryForbidsKeys(const std::string& target,
                                                   const std::vector<std::string>& keys) {
  for (const std::string& entry : QueryEntries(target)) {
    const std::string key = QueryKey(entry);
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
      return ::testing::AssertionFailure()
             << "forbidden query key \"" << key << "\" present in target \"" << target << "\"";
    }
  }
  return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult QueryRequiresKeys(const std::string& target,
                                                    const std::vector<std::string>& keys) {
  const std::vector<std::string> entries = QueryEntries(target);
  for (const std::string& key : keys) {
    const bool found = std::any_of(entries.begin(), entries.end(), [&](const std::string& entry) {
      return QueryKey(entry) == key;
    });
    if (!found) {
      return ::testing::AssertionFailure()
             << "required query key \"" << key << "\" missing from target \"" << target << "\"";
    }
  }
  return ::testing::AssertionSuccess();
}

// Structural Document equality for protocol bodies: numbers compare by value
// across the int/double representations the codecs may pick, and NaN equals
// NaN (protocol tests use NaN payloads; operator== would reject them).
inline bool ProtocolDocumentEquals(const Document& a, const Document& b) {
  const bool a_number = a.is_int() || a.is_double();
  const bool b_number = b.is_int() || b.is_double();
  if (a_number || b_number) {
    if (!a_number || !b_number) return false;
    if (a.is_int() && b.is_int()) return a.as_int() == b.as_int();
    const double x = a.AsNumber();
    const double y = b.AsNumber();
    return (std::isnan(x) && std::isnan(y)) || x == y;
  }
  if (a.is_list() && b.is_list()) {
    const auto& la = a.as_list();
    const auto& lb = b.as_list();
    if (la.size() != lb.size()) return false;
    for (std::size_t i = 0; i < la.size(); ++i) {
      if (!ProtocolDocumentEquals(la[i], lb[i])) return false;
    }
    return true;
  }
  if (a.is_map() && b.is_map()) {
    const auto& ma = a.as_map();
    const auto& mb = b.as_map();
    if (ma.size() != mb.size()) return false;
    for (const auto& [key, value] : ma) {
      const Document* other = b.Find(key);
      if (other == nullptr || !ProtocolDocumentEquals(value, *other)) return false;
    }
    return true;
  }
  return a == b;
}

inline ::testing::AssertionResult JsonBodyEquals(const std::string& expected,
                                                 const std::string& actual) {
  auto expected_doc = opal::json::Decode(expected);
  if (!expected_doc.ok()) {
    return ::testing::AssertionFailure()
           << "expected body is not valid JSON: " << expected_doc.error().message();
  }
  auto actual_doc = opal::json::Decode(actual);
  if (!actual_doc.ok()) {
    return ::testing::AssertionFailure()
           << "actual body is not valid JSON: " << actual_doc.error().message() << "\nbody: <<"
           << actual << ">>";
  }
  if (!ProtocolDocumentEquals(*expected_doc, *actual_doc)) {
    return ::testing::AssertionFailure()
           << "JSON bodies differ.\nexpected: " << expected << "\nactual:   " << actual;
  }
  return ::testing::AssertionSuccess();
}

// httpMalformedRequestTests `messageRegex` assertion: the pattern must match the
// "message" member of the (JSON) error body.
inline ::testing::AssertionResult BodyMessageMatches(const std::string& pattern,
                                                     const std::string& body) {
  auto doc = opal::json::Decode(body);
  if (!doc.ok() || !doc->is_map()) {
    return ::testing::AssertionFailure() << "body is not a JSON object: <<" << body << ">>";
  }
  const Document* message = doc->Find("message");
  if (message == nullptr || !message->is_string()) {
    return ::testing::AssertionFailure() << "body has no string \"message\": <<" << body << ">>";
  }
  if (!std::regex_search(message->as_string(), std::regex(pattern, std::regex::ECMAScript))) {
    return ::testing::AssertionFailure() << "message does not match.\npattern: " << pattern
                                         << "\nmessage: " << message->as_string();
  }
  return ::testing::AssertionSuccess();
}

// Decodes a base64 body (how binary bodies appear in test definitions).
inline std::string FromBase64(const std::string& text) {
  auto blob = opal::Base64Decode(text);
  return blob.ok() ? blob->ToString() : std::string();
}

inline ::testing::AssertionResult CborBodyEqualsBase64(const std::string& expected_base64,
                                                       const std::string& actual) {
  auto expected_bytes = opal::Base64Decode(expected_base64);
  if (!expected_bytes.ok()) {
    return ::testing::AssertionFailure() << "expected body is not valid base64";
  }
  auto expected_doc = opal::cbor::Decode(*expected_bytes);
  if (!expected_doc.ok()) {
    return ::testing::AssertionFailure()
           << "expected body is not valid CBOR: " << expected_doc.error().message();
  }
  auto actual_doc = opal::cbor::Decode(opal::Blob::FromString(actual));
  if (!actual_doc.ok()) {
    return ::testing::AssertionFailure()
           << "actual body is not valid CBOR: " << actual_doc.error().message();
  }
  if (!ProtocolDocumentEquals(*expected_doc, *actual_doc)) {
    return ::testing::AssertionFailure()
           << "CBOR bodies differ.\nexpected (b64): " << expected_base64
           << "\nactual (b64):   " << opal::Base64Encode(opal::Blob::FromString(actual));
  }
  return ::testing::AssertionSuccess();
}

}  // namespace opal::testing

#endif  // OPAL_TESTING_PROTOCOL_TEST_H_
