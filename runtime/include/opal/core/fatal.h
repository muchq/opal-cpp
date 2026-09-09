#ifndef OPAL_CORE_FATAL_H_
#define OPAL_CORE_FATAL_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace opal::internal {

// Terminates the process after printing `message` to stderr — the one abort
// primitive behind every contract violation (ADR-0009): exceptions never
// cross the public API (ADR-0003), so violations fail fast with context
// instead of throwing std::bad_variant_access with none. Cold and non-inline
// so the crash path never bloats the accessors that guard with it.
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] inline void Fatal(std::string_view message) {
  std::fprintf(stderr, "opal: %.*s\n", static_cast<int>(message.size()), message.data());
  std::abort();
}

// An Outcome accessed on the side it doesn't hold. `code` and `message`
// describe the held error; both are empty when the error type carries
// neither. Non-template so Outcome's header-inline accessors stay one
// branch + one call.
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] inline void FatalOutcomeError(
    std::string_view context, std::string_view code, std::string_view message) {
  std::string text(context);
  if (!code.empty() || !message.empty()) {
    text.append(": ").append(code).append(": ").append(message);
  }
  Fatal(text);
}

// A generated union's as_<requested>() called while `engaged` is the active
// member. Centralized so every generated accessor is one branch + one call.
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] inline void FatalWrongUnionAccess(
    const char* union_name, const char* requested, const char* engaged) {
  std::string text(union_name);
  text.append("::as_").append(requested).append("(): engaged member is ").append(engaged);
  Fatal(text);
}

}  // namespace opal::internal

#endif  // OPAL_CORE_FATAL_H_
