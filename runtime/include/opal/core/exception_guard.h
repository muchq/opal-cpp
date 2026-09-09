#ifndef OPAL_CORE_EXCEPTION_GUARD_H_
#define OPAL_CORE_EXCEPTION_GUARD_H_

#include <utility>

#if defined(__cpp_exceptions)
#include <exception>
#endif

namespace opal::internal {

// Runs `body()` and returns its result. If `body` throws, `on_throw` is
// invoked with the exception's what() message — or nullptr for a non-std
// throw — and its result is returned instead.
//
// This is the containment primitive behind ADR-0003 and ADR-0009: an
// exception must never cross a smithy Outcome boundary or unwind a transport
// io thread, so every wire-facing callback the runtime invokes on a
// completion context (metrics sinks, readiness probes, request handlers,
// stream callbacks) runs inside a Contain().
//
// When the translation unit is built with -fno-exceptions nothing *can*
// throw, so the handler is unreachable and `body`'s result is returned
// directly. That is what lets the dependency-light runtime compile clean
// under -fno-exceptions (the ADR-0003 enforcement gate) while retaining full
// containment under the default exceptions-enabled build — one helper, both
// postures, no divergent control flow at the call site.
//
// `on_throw` must return the same type as `body` (or both must be void).
template <typename Body, typename OnThrow>
auto Contain(Body&& body, OnThrow&& on_throw) -> decltype(std::forward<Body>(body)()) {
#if defined(__cpp_exceptions)
  try {
    return std::forward<Body>(body)();
  } catch (const std::exception& e) {
    return std::forward<OnThrow>(on_throw)(e.what());
  } catch (...) {
    return std::forward<OnThrow>(on_throw)(nullptr);
  }
#else
  static_cast<void>(on_throw);
  return std::forward<Body>(body)();
#endif
}

}  // namespace opal::internal

#endif  // OPAL_CORE_EXCEPTION_GUARD_H_
