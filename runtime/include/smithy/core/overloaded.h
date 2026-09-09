#ifndef SMITHY_CORE_OVERLOADED_H_
#define SMITHY_CORE_OVERLOADED_H_

namespace opal {

// Overload-set builder for visiting generated unions (or any std::variant):
//
//   status.visit(opal::Overloaded{
//       [](const PendingStatus& p) { ... },
//       [](const ReadyStatus& r) { ... },
//       [](const CancelledStatus&) { ... },
//       [](std::monostate) { ... },  // the union's empty state
//   });
template <typename... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

}  // namespace opal

#endif  // SMITHY_CORE_OVERLOADED_H_
