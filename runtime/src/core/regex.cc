#include "smithy/core/regex.h"

#include <memory>
#include <string>
#include <utility>

#include "smithy/core/error.h"

namespace opal {
namespace {

// Compiled programs are bounded so counted repetition ({n,m} expands by
// duplication) cannot balloon memory; patterns come from the model, so the
// caps only need to be generous, not tight.
constexpr std::size_t kMaxProgramSize = 1 << 16;
constexpr int kMaxRepeatCount = 1024;
constexpr int kMaxGroupDepth = 128;

bool IsWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::bitset<256> DigitClass() {
  std::bitset<256> s;
  for (int c = '0'; c <= '9'; ++c) s.set(c);
  return s;
}

std::bitset<256> WordClass() {
  std::bitset<256> s;
  for (int c = 0; c < 256; ++c) {
    if (IsWordByte(static_cast<unsigned char>(c))) s.set(c);
  }
  return s;
}

std::bitset<256> SpaceClass() {
  std::bitset<256> s;
  // ECMA WhiteSpace + LineTerminator, restricted to single bytes.
  for (unsigned char c : {' ', '\t', '\n', '\r', '\f', '\v'}) s.set(c);
  return s;
}

std::bitset<256> DotClass() {
  // '.' over bytes: everything except the single-byte line terminators,
  // matching the byte-oriented std::regex behavior this engine replaces.
  std::bitset<256> s;
  s.set();
  s.reset('\n');
  s.reset('\r');
  return s;
}

// Parse tree.
struct Node;
using NodePtr = std::unique_ptr<Node>;
struct Node {
  enum class Kind {
    kEmpty,   // matches the empty string
    kByte,    // one literal byte
    kClass,   // one byte from a set
    kConcat,  // children in sequence
    kAlt,     // any one child
    kRepeat,  // child repeated [min, max] times (max < 0 = unbounded)
    kAssert,  // zero-width assertion
  };
  explicit Node(Kind k) : kind(k) {}
  Kind kind;
  std::uint8_t byte = 0;
  std::bitset<256> cls;
  std::vector<NodePtr> children;
  int min = 0;
  int max = -1;
  Regex::Inst::Assert assert_kind = Regex::Inst::Assert::kInputStart;
};

NodePtr MakeByte(std::uint8_t b) {
  auto n = std::make_unique<Node>(Node::Kind::kByte);
  n->byte = b;
  return n;
}

NodePtr MakeClass(std::bitset<256> cls) {
  auto n = std::make_unique<Node>(Node::Kind::kClass);
  n->cls = cls;
  return n;
}

NodePtr MakeAssert(Regex::Inst::Assert kind) {
  auto n = std::make_unique<Node>(Node::Kind::kAssert);
  n->assert_kind = kind;
  return n;
}

// Recursive-descent parser over the pattern bytes.
class Parser {
 public:
  explicit Parser(std::string_view pattern) : pattern_(pattern) {}

  Outcome<NodePtr> Parse() {
    auto node = ParseAlternation(0);
    if (!node) return std::move(node).error();
    if (!AtEnd()) return Fail("unbalanced ')'");
    return node;
  }

 private:
  bool AtEnd() const { return pos_ >= pattern_.size(); }
  unsigned char Peek() const { return static_cast<unsigned char>(pattern_[pos_]); }
  unsigned char Take() { return static_cast<unsigned char>(pattern_[pos_++]); }
  bool Eat(char c) {
    if (AtEnd() || pattern_[pos_] != c) return false;
    ++pos_;
    return true;
  }

  static Error Fail(const std::string& why) { return Error::Serialization("Regex: " + why); }

  Outcome<NodePtr> ParseAlternation(int depth) {
    auto first = ParseConcat(depth);
    if (!first) return first;
    if (AtEnd() || Peek() != '|') return first;
    auto alt = std::make_unique<Node>(Node::Kind::kAlt);
    alt->children.push_back(std::move(*first));
    while (Eat('|')) {
      auto branch = ParseConcat(depth);
      if (!branch) return branch;
      alt->children.push_back(std::move(*branch));
    }
    return NodePtr(std::move(alt));
  }

  Outcome<NodePtr> ParseConcat(int depth) {
    auto concat = std::make_unique<Node>(Node::Kind::kConcat);
    while (!AtEnd() && Peek() != '|' && Peek() != ')') {
      auto piece = ParseRepeat(depth);
      if (!piece) return piece;
      concat->children.push_back(std::move(*piece));
    }
    if (concat->children.empty()) return NodePtr(std::make_unique<Node>(Node::Kind::kEmpty));
    if (concat->children.size() == 1) return std::move(concat->children.front());
    return NodePtr(std::move(concat));
  }

  Outcome<NodePtr> ParseRepeat(int depth) {
    auto atom = ParseAtom(depth);
    if (!atom) return atom;
    int min = 0;
    int max = -1;
    unsigned char q = AtEnd() ? 0 : Peek();
    if (q == '*') {
      ++pos_;
    } else if (q == '+') {
      ++pos_;
      min = 1;
    } else if (q == '?') {
      ++pos_;
      max = 1;
    } else if (q == '{') {
      std::size_t saved = pos_;
      if (!ParseBoundedQuantifier(&min, &max)) {
        // Not a well-formed {n,m} quantifier: '{' is a literal (the lenient
        // Annex-B behavior real engines implement).
        pos_ = saved;
        return atom;
      }
    } else {
      return atom;
    }
    if ((*atom)->kind == Node::Kind::kAssert) {
      return Fail("quantifier applied to an assertion");
    }
    Eat('?');  // Lazy quantifiers exist-match identically; accept and ignore.
    if (!AtEnd() && (Peek() == '*' || Peek() == '+')) {
      return Fail("double quantifier");
    }
    auto repeat = std::make_unique<Node>(Node::Kind::kRepeat);
    repeat->min = min;
    repeat->max = max;
    repeat->children.push_back(std::move(*atom));
    return NodePtr(std::move(repeat));
  }

  // Parses {n}, {n,}, {n,m} starting at '{'; false when not a quantifier.
  bool ParseBoundedQuantifier(int* min, int* max) {
    ++pos_;  // '{'
    int n = 0;
    if (!ParseInt(&n)) return false;
    if (Eat('}')) {
      *min = n;
      *max = n;
      return true;
    }
    if (!Eat(',')) return false;
    if (Eat('}')) {
      *min = n;
      *max = -1;
      return true;
    }
    int m = 0;
    if (!ParseInt(&m) || !Eat('}') || m < n) return false;
    *min = n;
    *max = m;
    return true;
  }

  bool ParseInt(int* out) {
    if (AtEnd() || Peek() < '0' || Peek() > '9') return false;
    long value = 0;
    while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
      value = value * 10 + (Take() - '0');
      if (value > kMaxRepeatCount) return false;
    }
    *out = static_cast<int>(value);
    return true;
  }

  Outcome<NodePtr> ParseAtom(int depth) {
    unsigned char c = Take();
    switch (c) {
      case '^':
        return MakeAssert(Regex::Inst::Assert::kInputStart);
      case '$':
        return MakeAssert(Regex::Inst::Assert::kInputEnd);
      case '.':
        return MakeClass(DotClass());
      case '(':
        return ParseGroup(depth);
      case '[':
        return ParseClass();
      case '\\':
        return ParseEscape(/*in_class=*/false);
      case '*':
      case '+':
      case '?':
        return Fail("quantifier with nothing to repeat");
      default:
        return MakeByte(c);
    }
  }

  Outcome<NodePtr> ParseGroup(int depth) {
    if (depth >= kMaxGroupDepth) return Fail("groups nested too deeply");
    if (Eat('?')) {
      if (Eat(':')) {
        // Non-capturing group.
      } else if (!AtEnd() && (Peek() == '=' || Peek() == '!')) {
        return Fail("lookahead is not supported by the linear-time engine");
      } else if (Eat('<')) {
        if (!AtEnd() && (Peek() == '=' || Peek() == '!')) {
          return Fail("lookbehind is not supported by the linear-time engine");
        }
        // Named capturing group: skip the name, match as a plain group.
        while (!AtEnd() && Peek() != '>') ++pos_;
        if (!Eat('>')) return Fail("unterminated group name");
      } else {
        return Fail("unsupported group syntax '(?'");
      }
    }
    auto body = ParseAlternation(depth + 1);
    if (!body) return body;
    if (!Eat(')')) return Fail("missing ')'");
    return body;
  }

  Outcome<NodePtr> ParseClass() {
    std::bitset<256> cls;
    bool negate = Eat('^');
    bool first = true;
    while (true) {
      if (AtEnd()) return Fail("unterminated character class");
      if (Peek() == ']' && !first) {
        ++pos_;
        break;
      }
      first = false;
      // One class member: a literal byte or an escape (which may itself be a
      // whole class, e.g. [\d-] — a class escape cannot start a range).
      std::bitset<256> lo_class;
      int lo = -1;
      unsigned char c = Take();
      if (c == '\\') {
        auto escaped = ParseClassEscape(&lo_class, &lo);
        if (!escaped.ok()) return std::move(escaped).error();
      } else {
        lo = c;
      }
      if (lo < 0) {
        cls |= lo_class;
        continue;
      }
      if (!AtEnd() && Peek() == '-' && pos_ + 1 < pattern_.size() && pattern_[pos_ + 1] != ']') {
        ++pos_;  // '-'
        int hi = -1;
        std::bitset<256> hi_class;
        unsigned char h = Take();
        if (h == '\\') {
          auto escaped = ParseClassEscape(&hi_class, &hi);
          if (!escaped.ok()) return std::move(escaped).error();
        } else {
          hi = h;
        }
        if (hi < 0) return Fail("class escape cannot end a range");
        if (hi < lo) return Fail("character range is out of order");
        for (int b = lo; b <= hi; ++b) cls.set(static_cast<std::size_t>(b));
      } else {
        cls.set(static_cast<std::size_t>(lo));
      }
    }
    if (negate) cls.flip();
    return MakeClass(cls);
  }

  // After a backslash inside a class: either a single byte (*byte >= 0) or a
  // class escape (*byte stays -1 and *cls is filled in).
  Outcome<Unit> ParseClassEscape(std::bitset<256>* cls, int* byte) {
    if (AtEnd()) return Fail("dangling escape");
    unsigned char c = Take();
    switch (c) {
      case 'd':
        *cls = DigitClass();
        return Unit{};
      case 'D':
        *cls = ~DigitClass();
        return Unit{};
      case 'w':
        *cls = WordClass();
        return Unit{};
      case 'W':
        *cls = ~WordClass();
        return Unit{};
      case 's':
        *cls = SpaceClass();
        return Unit{};
      case 'S':
        *cls = ~SpaceClass();
        return Unit{};
      case 'b':
        *byte = 0x08;
        return Unit{};  // \b inside a class = backspace
      default: {
        int b = SimpleEscapeByte(c);
        if (b < 0) return Fail(std::string("unsupported escape '\\") + static_cast<char>(c) + "'");
        *byte = b;
        return Unit{};
      }
    }
  }

  Outcome<NodePtr> ParseEscape(bool /*in_class*/) {
    if (AtEnd()) return Fail("dangling escape");
    unsigned char c = Take();
    switch (c) {
      case 'd':
        return MakeClass(DigitClass());
      case 'D':
        return MakeClass(~DigitClass());
      case 'w':
        return MakeClass(WordClass());
      case 'W':
        return MakeClass(~WordClass());
      case 's':
        return MakeClass(SpaceClass());
      case 'S':
        return MakeClass(~SpaceClass());
      case 'b':
        return MakeAssert(Regex::Inst::Assert::kWordBoundary);
      case 'B':
        return MakeAssert(Regex::Inst::Assert::kNotWordBoundary);
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        return Fail("backreferences are not supported by the linear-time engine");
      case 'u': {
        long code = ParseHex(4);
        if (code < 0) return Fail("malformed \\uHHHH escape");
        return Utf8Literal(code);
      }
      default: {
        int b = SimpleEscapeByte(c);
        if (b < 0) return Fail(std::string("unsupported escape '\\") + static_cast<char>(c) + "'");
        return MakeByte(static_cast<std::uint8_t>(b));
      }
    }
  }

  // Escapes shared by class and non-class contexts; -1 when unknown.
  int SimpleEscapeByte(unsigned char c) {
    switch (c) {
      case 'n':
        return '\n';
      case 'r':
        return '\r';
      case 't':
        return '\t';
      case 'f':
        return '\f';
      case 'v':
        return '\v';
      case '0':
        return '\0';
      case 'x': {
        long b = ParseHex(2);
        return b < 0 ? -1 : static_cast<int>(b);
      }
      default:
        // ECMA identity escapes: any non-alphanumeric escapes to itself.
        if (IsWordByte(c)) return -1;
        return c;
    }
  }

  long ParseHex(int digits) {
    long value = 0;
    for (int i = 0; i < digits; ++i) {
      if (AtEnd()) return -1;
      unsigned char c = Take();
      int d = -1;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        d = c - 'A' + 10;
      if (d < 0) return -1;
      value = value * 16 + d;
    }
    return value;
  }

  // A \uHHHH escape matches the code point's UTF-8 byte sequence.
  static Outcome<NodePtr> Utf8Literal(long code) {
    if (code >= 0xD800 && code <= 0xDFFF) {
      return Fail("surrogate \\u escapes are not supported");
    }
    if (code < 0x80) return MakeByte(static_cast<std::uint8_t>(code));
    auto concat = std::make_unique<Node>(Node::Kind::kConcat);
    if (code < 0x800) {
      concat->children.push_back(MakeByte(static_cast<std::uint8_t>(0xC0 | (code >> 6))));
    } else {
      concat->children.push_back(MakeByte(static_cast<std::uint8_t>(0xE0 | (code >> 12))));
      concat->children.push_back(MakeByte(static_cast<std::uint8_t>(0x80 | ((code >> 6) & 0x3F))));
    }
    concat->children.push_back(MakeByte(static_cast<std::uint8_t>(0x80 | (code & 0x3F))));
    return NodePtr(std::move(concat));
  }

  std::string_view pattern_;
  std::size_t pos_ = 0;
};

}  // namespace

// Emits the parse tree as a Thompson NFA program.
class RegexCompiler {
 public:
  static Outcome<Regex> Compile(std::string_view pattern) {
    Parser parser(pattern);
    auto tree = parser.Parse();
    if (!tree) return std::move(tree).error();
    Regex re;
    RegexCompiler compiler(&re);
    if (auto emitted = compiler.Emit(**tree); !emitted.ok()) {
      return std::move(emitted).error();
    }
    if (auto added = compiler.Add({Regex::Inst::Op::kMatch}); !added.ok()) {
      return std::move(added).error();
    }
    return re;
  }

 private:
  using Inst = Regex::Inst;

  explicit RegexCompiler(Regex* re) : re_(re) {}

  Outcome<Unit> Add(Inst inst) {
    if (re_->program_.size() >= kMaxProgramSize) {
      return Error::Serialization("Regex: pattern compiles to too large a program");
    }
    re_->program_.push_back(inst);
    return Unit{};
  }

  std::uint32_t Here() const { return static_cast<std::uint32_t>(re_->program_.size()); }

  std::uint32_t AddClass(const std::bitset<256>& cls) {
    for (std::size_t i = 0; i < re_->classes_.size(); ++i) {
      if (re_->classes_[i] == cls) return static_cast<std::uint32_t>(i);
    }
    re_->classes_.push_back(cls);
    return static_cast<std::uint32_t>(re_->classes_.size() - 1);
  }

  Outcome<Unit> Emit(const Node& node) {
    switch (node.kind) {
      case Node::Kind::kEmpty:
        return Unit{};
      case Node::Kind::kByte: {
        Inst inst{Inst::Op::kByte};
        inst.byte = node.byte;
        return Add(inst);
      }
      case Node::Kind::kClass: {
        Inst inst{Inst::Op::kClass};
        inst.arg = AddClass(node.cls);
        return Add(inst);
      }
      case Node::Kind::kAssert: {
        Inst inst{Inst::Op::kAssert};
        inst.assert_kind = node.assert_kind;
        return Add(inst);
      }
      case Node::Kind::kConcat:
        for (const NodePtr& child : node.children) {
          if (auto emitted = Emit(*child); !emitted.ok()) return emitted;
        }
        return Unit{};
      case Node::Kind::kAlt: {
        // split; A; jmp end; split; B; jmp end; ...; C
        std::vector<std::uint32_t> jumps_to_end;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
          const bool last = i + 1 == node.children.size();
          std::uint32_t split_pc = 0;
          if (!last) {
            split_pc = Here();
            if (auto added = Add({Inst::Op::kSplit}); !added.ok()) return added;
          }
          if (auto emitted = Emit(*node.children[i]); !emitted.ok()) return emitted;
          if (!last) {
            jumps_to_end.push_back(Here());
            if (auto added = Add({Inst::Op::kJmp}); !added.ok()) return added;
            re_->program_[split_pc].arg = Here();
          }
        }
        for (std::uint32_t pc : jumps_to_end) re_->program_[pc].arg = Here();
        return Unit{};
      }
      case Node::Kind::kRepeat:
        return EmitRepeat(node);
    }
    return Unit{};  // Unreachable; keeps -Werror switch analysis happy.
  }

  Outcome<Unit> EmitRepeat(const Node& node) {
    const Node& body = *node.children.front();
    for (int i = 0; i < node.min; ++i) {
      if (auto emitted = Emit(body); !emitted.ok()) return emitted;
    }
    if (node.max < 0) {
      // body{min,} — one looping optional copy: L: split(end); body; jmp L
      std::uint32_t loop = Here();
      if (auto added = Add({Inst::Op::kSplit}); !added.ok()) return added;
      if (auto emitted = Emit(body); !emitted.ok()) return emitted;
      Inst jmp{Inst::Op::kJmp};
      jmp.arg = loop;
      if (auto added = Add(jmp); !added.ok()) return added;
      re_->program_[loop].arg = Here();
      return Unit{};
    }
    // body{min,max} — (max - min) nested optional copies: each split jumps
    // past everything that remains.
    std::vector<std::uint32_t> splits;
    for (int i = node.min; i < node.max; ++i) {
      splits.push_back(Here());
      if (auto added = Add({Inst::Op::kSplit}); !added.ok()) return added;
      if (auto emitted = Emit(body); !emitted.ok()) return emitted;
    }
    for (std::uint32_t pc : splits) re_->program_[pc].arg = Here();
    return Unit{};
  }

  Regex* re_;
};

Outcome<Regex> Regex::Compile(std::string_view pattern) { return RegexCompiler::Compile(pattern); }

bool Regex::AddThread(std::vector<std::uint32_t>* list, std::vector<std::uint32_t>* seen_stamp,
                      std::uint32_t stamp, std::uint32_t pc, std::string_view text, std::size_t pos,
                      std::size_t* steps) const {
  // Iterative epsilon closure; the explicit stack keeps deeply split
  // programs from overflowing the call stack.
  std::vector<std::uint32_t> work{pc};
  while (!work.empty()) {
    std::uint32_t at = work.back();
    work.pop_back();
    if ((*seen_stamp)[at] == stamp) continue;
    (*seen_stamp)[at] = stamp;
    if (steps != nullptr) ++*steps;
    const Inst& inst = program_[at];
    switch (inst.op) {
      case Inst::Op::kMatch:
        return true;
      case Inst::Op::kJmp:
        work.push_back(inst.arg);
        break;
      case Inst::Op::kSplit:
        work.push_back(at + 1);
        work.push_back(inst.arg);
        break;
      case Inst::Op::kAssert: {
        bool before = pos > 0 && IsWordByte(static_cast<unsigned char>(text[pos - 1]));
        bool after = pos < text.size() && IsWordByte(static_cast<unsigned char>(text[pos]));
        bool holds = false;
        switch (inst.assert_kind) {
          case Inst::Assert::kInputStart:
            holds = pos == 0;
            break;
          case Inst::Assert::kInputEnd:
            holds = pos == text.size();
            break;
          case Inst::Assert::kWordBoundary:
            holds = before != after;
            break;
          case Inst::Assert::kNotWordBoundary:
            holds = before == after;
            break;
        }
        if (holds) work.push_back(at + 1);
        break;
      }
      case Inst::Op::kByte:
      case Inst::Op::kClass:
        list->push_back(at);
        break;
    }
  }
  return false;
}

bool Regex::Search(std::string_view text) const { return Search(text, nullptr); }

bool Regex::Search(std::string_view text, std::size_t* steps) const {
  if (steps != nullptr) *steps = 0;
  if (program_.empty()) return false;
  std::vector<std::uint32_t> current;
  std::vector<std::uint32_t> next;
  // Stamps deduplicate threads per input position without clearing a
  // visited set on every byte. 0 means "never seen"; stamps start at 1.
  std::vector<std::uint32_t> seen_stamp(program_.size(), 0);
  for (std::size_t pos = 0; pos <= text.size(); ++pos) {
    // Unanchored search: a fresh attempt starts at every position.
    if (AddThread(&current, &seen_stamp, static_cast<std::uint32_t>(pos) + 1, 0, text, pos,
                  steps)) {
      return true;
    }
    if (pos == text.size()) break;
    const auto c = static_cast<unsigned char>(text[pos]);
    next.clear();
    for (std::uint32_t pc : current) {
      const Inst& inst = program_[pc];
      bool matches = inst.op == Inst::Op::kByte ? inst.byte == c : classes_[inst.arg].test(c);
      if (matches && AddThread(&next, &seen_stamp, static_cast<std::uint32_t>(pos) + 2, pc + 1,
                               text, pos + 1, steps)) {
        return true;
      }
    }
    current.swap(next);
  }
  return false;
}

}  // namespace opal
