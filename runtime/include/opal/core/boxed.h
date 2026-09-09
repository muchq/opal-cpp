#ifndef OPAL_CORE_BOXED_H_
#define OPAL_CORE_BOXED_H_

#include <memory>
#include <string>
#include <utility>

#include "opal/core/print.h"

namespace opal {

// Value-semantic heap indirection for recursive generated members (a
// structure member whose target refers back to its container). Copy is a
// deep copy and equality compares the pointed-to values, so generated
// structs keep their defaulted copy/equality semantics. Never null except
// moved-from; using a moved-from Boxed (other than assigning to it or
// destroying it) is undefined behavior.
//
// The template compiles with an incomplete T: member bodies only
// instantiate at their call sites, where the generated types header has
// completed every type.
template <typename T>
class Boxed {
 public:
  Boxed() : value_(std::make_unique<T>()) {}
  Boxed(T value) : value_(std::make_unique<T>(std::move(value))) {}  // NOLINT
  Boxed(const Boxed& other) : value_(std::make_unique<T>(*other.value_)) {}
  Boxed(Boxed&&) noexcept = default;
  Boxed& operator=(const Boxed& other) {
    if (this != &other) value_ = std::make_unique<T>(*other.value_);
    return *this;
  }
  Boxed& operator=(Boxed&&) noexcept = default;
  Boxed& operator=(T value) {
    value_ = std::make_unique<T>(std::move(value));
    return *this;
  }
  ~Boxed() = default;

  T& operator*() { return *value_; }
  const T& operator*() const { return *value_; }
  T* operator->() { return value_.get(); }
  const T* operator->() const { return value_.get(); }

  friend bool operator==(const Boxed& a, const Boxed& b) { return *a.value_ == *b.value_; }
  friend bool operator!=(const Boxed& a, const Boxed& b) { return !(a == b); }

  // Debug rendering: the box is invisible — prints the value. Data is
  // acyclic (value semantics), so recursion through the box terminates.
  void AppendDebugTo(std::string& out) const { DebugAppend(out, *value_); }
  // Deliberately NO operator<=>: an auto-returning deep <=> must deduce its
  // type through the recursive chain Boxed exists to break, and clang
  // hard-errors on the cycle (std::optional's constraint check instantiates
  // it eagerly). Without one, a recursive struct's defaulted <=> is cleanly
  // deleted instead: recursion keeps deep equality only.

 private:
  std::unique_ptr<T> value_;
};

}  // namespace opal

#endif  // OPAL_CORE_BOXED_H_
