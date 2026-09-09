#ifndef OPAL_CORE_ERROR_H_
#define OPAL_CORE_ERROR_H_

#include <any>
#include <string>
#include <utility>

namespace opal {

// Broad classification of a failure, used for retry and reporting decisions.
enum class ErrorKind {
  kTransport,      // Connection, DNS, timeout — the request may not have been seen.
  kSerialization,  // Malformed or unexpected payload on either side.
  kValidation,     // Input rejected before reaching business logic.
  kModeled,        // An error shape modeled in the Smithy service definition.
  kUnknown,
};

// A failure carried through Outcome<T> (ADR-0003). Exceptions never cross the
// public API boundary; every failure is represented as a value of this type.
//
//   auto outcome = client.GetForecast(input);
//   if (!outcome) log(outcome.error().message());
class Error {
 public:
  Error() = default;
  Error(ErrorKind kind, std::string code, std::string message, bool retryable = false)
      : kind_(kind), code_(std::move(code)), message_(std::move(message)), retryable_(retryable) {}

  static Error Transport(std::string message, bool retryable = true) {
    return Error(ErrorKind::kTransport, "TransportError", std::move(message), retryable);
  }
  // A deadline the caller set expired before the operation could finish —
  // a transport failure whose own code separates it from a broken wire
  // (WebSocket::Receive's timed overload leans on exactly that: nothing
  // arrived yet, and the session is still usable). Retryable in the sense
  // SendWithRetries means it — a timed-out attempt may be reattempted —
  // which for a stream receive means "wait again on the same session", NOT
  // "restart the operation": the session outlived the deadline, so there is
  // nothing to redo. Branch on code() == "TimeoutError", not on retryable().
  static Error Timeout(std::string message) {
    return Error(ErrorKind::kTransport, "TimeoutError", std::move(message), /*retryable=*/true);
  }
  static Error Serialization(std::string message) {
    return Error(ErrorKind::kSerialization, "SerializationError", std::move(message));
  }
  static Error Validation(std::string message) {
    return Error(ErrorKind::kValidation, "ValidationError", std::move(message));
  }
  static Error Modeled(std::string code, std::string message, bool retryable = false) {
    return Error(ErrorKind::kModeled, std::move(code), std::move(message), retryable);
  }
  static Error Unknown(std::string message) {
    return Error(ErrorKind::kUnknown, "UnknownError", std::move(message));
  }

  ErrorKind kind() const { return kind_; }
  const std::string& code() const { return code_; }
  const std::string& message() const { return message_; }
  bool retryable() const { return retryable_; }

  // Typed payload for modeled errors: generated clients attach the
  // deserialized error structure so callers can recover it without re-parsing.
  //
  //   if (const auto* e = outcome.error().detail<OrderNotFound>()) use(e->orderId);
  void set_detail(std::any detail) { detail_ = std::move(detail); }
  bool has_detail() const { return detail_.has_value(); }
  template <typename T>
  const T* detail() const {
    return std::any_cast<T>(&detail_);
  }

  // Equality ignores detail(): two errors with the same classification compare
  // equal whether or not a typed payload was attached.
  friend bool operator==(const Error& a, const Error& b) {
    return a.kind_ == b.kind_ && a.code_ == b.code_ && a.message_ == b.message_ &&
           a.retryable_ == b.retryable_;
  }

 private:
  ErrorKind kind_ = ErrorKind::kUnknown;
  std::string code_ = "UnknownError";
  std::string message_;
  bool retryable_ = false;
  std::any detail_;
};

}  // namespace opal

#endif  // OPAL_CORE_ERROR_H_
