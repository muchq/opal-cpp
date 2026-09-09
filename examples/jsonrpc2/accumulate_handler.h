#ifndef OPAL_EXAMPLES_JSONRPC2_ACCUMULATE_HANDLER_H_
#define OPAL_EXAMPLES_JSONRPC2_ACCUMULATE_HANDLER_H_

// The reference Calculator handlers every stream suite drives — the
// in-memory pair e2e and the Beast e2e, blocking and async — one place so
// the seams cannot drift. Accumulate seeds the running total from the
// opening call's `start`, answers each non-zero term with the new total, a
// zero term ends the session cleanly (the "=" key), and passing
// kAccumulateLimit ends it with the modeled Overflow — the terminal error
// envelope on the wire (ADR-0023).

#include <string>

#include "example/calculator/server.h"
#include "opal/core/outcome.h"

namespace example::calculator {

inline constexpr double kAccumulateLimit = 100.0;

inline opal::Error MakeOverflowError() {
  opal::Error overflow = opal::Error::Modeled("Overflow", "accumulator over the limit");
  overflow.set_detail(Overflow{.message = "accumulator over the limit", .limit = kAccumulateLimit});
  return overflow;
}

class AccumulatingCalculator final : public CalculatorHandler {
 public:
  opal::Outcome<AddOutput> Add(const AddInput& input,
                               const opal::server::RequestContext&) override {
    return AddOutput{.sum = input.a + input.b};
  }

  opal::Outcome<DivideOutput> Divide(const DivideInput& input,
                                     const opal::server::RequestContext&) override {
    if (input.divisor == 0) {
      opal::Error error = opal::Error::Modeled("DivisionByZero", "division by zero");
      error.set_detail(DivisionByZero{.message = "division by zero"});
      return error;
    }
    return DivideOutput{.quotient = input.dividend / input.divisor};
  }

  opal::Outcome<opal::Unit> Accumulate(const AccumulateInput& input, AccumulateServerStream& stream,
                                       const opal::server::RequestContext&) override {
    double total = input.start.value_or(0.0);
    while (true) {
      auto term = stream.Receive();
      if (!term.ok() || !term->has_value()) return opal::Unit{};  // wire failed or client left
      const double value = (**term).as_add().value;
      if (value == 0) return opal::Unit{};  // the "=" key: clean end, terminal result
      total += value;
      if (total > kAccumulateLimit) return MakeOverflowError();
      if (!stream.Send(Totals::FromTotal(RunningTotal{.value = total})).ok()) {
        return opal::Unit{};
      }
    }
  }
};

class AsyncAccumulatingCalculator final : public CalculatorAsyncHandler {
 public:
  opal::Outcome<AddOutput> Add(const AddInput& input,
                               const opal::server::RequestContext&) override {
    return AddOutput{.sum = input.a + input.b};
  }

  opal::Outcome<DivideOutput> Divide(const DivideInput& input,
                                     const opal::server::RequestContext&) override {
    if (input.divisor == 0) {
      opal::Error error = opal::Error::Modeled("DivisionByZero", "division by zero");
      error.set_detail(DivisionByZero{.message = "division by zero"});
      return error;
    }
    return DivideOutput{.quotient = input.dividend / input.divisor};
  }

  opal::eventstream::StreamTask Accumulate(AccumulateInput input,
                                           AccumulateAsyncServerStream& stream) override {
    double total = input.start.value_or(0.0);
    while (true) {
      auto term = co_await stream.Receive();
      if (!term.ok() || !term->has_value()) co_return opal::Unit{};
      const double value = (**term).as_add().value;
      if (value == 0) co_return opal::Unit{};
      total += value;
      if (total > kAccumulateLimit) co_return MakeOverflowError();
      auto sent = co_await stream.Send(Totals::FromTotal(RunningTotal{.value = total}));
      if (!sent.ok()) co_return opal::Unit{};
    }
  }
};

}  // namespace example::calculator

#endif  // OPAL_EXAMPLES_JSONRPC2_ACCUMULATE_HANDLER_H_
