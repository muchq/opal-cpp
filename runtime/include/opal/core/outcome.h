#ifndef OPAL_CORE_OUTCOME_H_
#define OPAL_CORE_OUTCOME_H_

#include <compare>
#include <concepts>
#include <cstddef>
#include <functional>
#include <string_view>
#include <utility>
#include <variant>

#include "opal/core/error.h"
#include "opal/core/fatal.h"

namespace opal {

// Marker value for operations that succeed without producing anything.
struct Unit {
  friend bool operator==(Unit /*lhs*/, Unit /*rhs*/) { return true; }
  // Trivially ordered (one value), so Unit union members don't block a
  // generated type's defaulted operator<=> (issue #49).
  friend std::strong_ordering operator<=>(Unit /*lhs*/, Unit /*rhs*/) {
    return std::strong_ordering::equal;
  }
  // Debug rendering (opal/core/print.h). Static is fine: the detection in
  // DebugAppend calls it through an object expression either way.
  static void AppendDebugTo(std::string& out) { out += "Unit{}"; }
};

// Result of an operation that can fail: holds either a value T or an error E.
// std::expected-like; the representation switches to std::expected once the
// compiler floor reaches C++23 (ADR-0003).
//
//   Outcome<int> Parse(std::string_view s);
//   if (auto n = Parse(s)) use(*n); else log(n.error().message());
//
// Accessing the side that isn't held is a contract violation and terminates
// the process with the error's code and message (ADR-0009) — never a
// context-free std::bad_variant_access. Prefer value_or_die("context") over
// a bare deref when there is no ok() check in sight: the context string
// lands on the crash line.
template <typename T, typename E = Error>
class Outcome {
 public:
  Outcome(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Outcome(E error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

  bool ok() const { return storage_.index() == 0; }
  explicit operator bool() const { return ok(); }

  // Precondition for value()/operator*/operator->: ok() is true.
  T& value() & { return value_or_die("Outcome::value() on error"); }
  const T& value() const& { return value_or_die("Outcome::value() on error"); }
  T&& value() && { return std::move(*this).value_or_die("Outcome::value() on error"); }

  // value() with caller-supplied context: dies as
  // "<context>: <code>: <message>" when the outcome holds an error.
  //
  //   auto client = CafeClient::Create(cfg).value_or_die("creating cafe client");
  T& value_or_die(std::string_view context) & {
    RequireValue(context);
    return std::get<0>(storage_);
  }
  const T& value_or_die(std::string_view context) const& {
    RequireValue(context);
    return std::get<0>(storage_);
  }
  T&& value_or_die(std::string_view context) && {
    RequireValue(context);
    return std::get<0>(std::move(storage_));
  }

  // Precondition: ok() is false.
  E& error() & {
    RequireError();
    return std::get<1>(storage_);
  }
  const E& error() const& {
    RequireError();
    return std::get<1>(storage_);
  }
  E&& error() && {
    RequireError();
    return std::get<1>(std::move(storage_));
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

  template <typename U>
  T value_or(U&& fallback) const& {
    return ok() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

  friend bool operator==(const Outcome& a, const Outcome& b) { return a.storage_ == b.storage_; }

 private:
  void RequireValue(std::string_view context) const {
    if (ok()) return;
    if constexpr (requires(const E& e) {
                    { e.code() } -> std::convertible_to<std::string_view>;
                    { e.message() } -> std::convertible_to<std::string_view>;
                  }) {
      const E& e = std::get<1>(storage_);
      internal::FatalOutcomeError(context, e.code(), e.message());
    } else {
      internal::FatalOutcomeError(context, {}, {});
    }
  }

  void RequireError() const {
    if (ok()) internal::Fatal("Outcome::error() called on a value");
  }

  std::variant<T, E> storage_;
};

}  // namespace opal

// One value, one hash — so Unit union members don't block a generated type's
// std::hash, mirroring Unit's trivial operator<=> (issue #49).
template <>
struct std::hash<opal::Unit> {
  std::size_t operator()(opal::Unit /*unit*/) const noexcept { return 0; }
};

#endif  // OPAL_CORE_OUTCOME_H_
