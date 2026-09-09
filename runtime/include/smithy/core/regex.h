#ifndef SMITHY_CORE_REGEX_H_
#define SMITHY_CORE_REGEX_H_

#include <bitset>
#include <cstdint>
#include <string_view>
#include <vector>

#include "smithy/core/outcome.h"

namespace opal {

// Linear-time regular expressions for generated @pattern validation. The
// pattern compiles to a Thompson NFA that Search() simulates breadth-first
// (a Pike VM), so matching costs O(program size x input bytes) for every
// pattern/input combination — a deliberately catastrophic pattern like
// ^([0-9]+)+$ evaluates in linear time instead of hanging the dispatch
// thread the way a backtracking engine does (ReDoS).
//
// Supported syntax is the ECMA-262 subset Smithy @pattern uses, evaluated
// over UTF-8 bytes exactly like the std::regex engine this replaces:
// literals, '.', character classes (ranges, negation, class escapes),
// \d \D \w \W \s \S, \xHH and \uHHHH escapes, ^ $ \b \B assertions,
// greedy and lazy quantifiers (* + ? {n} {n,} {n,m}), groups (capturing,
// non-capturing, named), and alternation. Constructs a linear-time engine
// cannot support — backreferences and lookaround — are Compile() errors;
// the code generator rejects such patterns before any C++ exists.
class Regex {
 public:
  // Compiles the pattern; kSerialization error on invalid syntax or on an
  // unsupported construct.
  static Outcome<Regex> Compile(std::string_view pattern);

  // True when text contains a match, std::regex_search semantics: partial
  // match anywhere in text unless the pattern anchors itself with ^/$.
  bool Search(std::string_view text) const;

  // Test instrumentation, not part of the supported API: Search while
  // counting VM work (instructions processed, epsilon closures included).
  // The stamp-based dedup runs each instruction at most once per input
  // position, so steps <= ProgramSize() x (text.size() + 2) for every
  // pattern/input pair — the linear-time property as a deterministic
  // assertion instead of a wall-clock bound.
  bool Search(std::string_view text, std::size_t* steps) const;
  std::size_t ProgramSize() const { return program_.size(); }

  // Implementation detail, public only so the compiler/parser in regex.cc
  // can name it; not part of the supported API.
  struct Inst {
    enum class Op : std::uint8_t {
      kByte,    // match one input byte equal to `byte`
      kClass,   // match one input byte in classes_[arg]
      kSplit,   // continue at both pc+1 and arg
      kJmp,     // continue at arg
      kAssert,  // zero-width check `assert_kind`, continue at pc+1
      kMatch,   // a match exists
    };
    enum class Assert : std::uint8_t {
      kInputStart,       // ^
      kInputEnd,         // $
      kWordBoundary,     // \b
      kNotWordBoundary,  // \B
    };
    Op op;
    Assert assert_kind = Assert::kInputStart;
    std::uint8_t byte = 0;
    std::uint32_t arg = 0;
  };

 private:
  Regex() = default;

  // Adds pc (following epsilon transitions and pos-applicable assertions) to
  // the thread list; returns true when a kMatch instruction is reached.
  // steps, when non-null, counts each instruction processed.
  bool AddThread(std::vector<std::uint32_t>* list, std::vector<std::uint32_t>* seen_stamp,
                 std::uint32_t stamp, std::uint32_t pc, std::string_view text, std::size_t pos,
                 std::size_t* steps) const;

  std::vector<Inst> program_;
  std::vector<std::bitset<256>> classes_;

  friend class RegexCompiler;
};

}  // namespace opal

#endif  // SMITHY_CORE_REGEX_H_
