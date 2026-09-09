#include "opal/core/regex.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <regex>
#include <string>

namespace {

bool Search(const std::string& pattern, const std::string& text) {
  auto re = opal::Regex::Compile(pattern);
  EXPECT_TRUE(re.ok()) << pattern << ": " << (re.ok() ? "" : re.error().message());
  return re.ok() && re->Search(text);
}

bool Compiles(const std::string& pattern) { return opal::Regex::Compile(pattern).ok(); }

TEST(RegexTest, LiteralsUsePartialMatchSemantics) {
  EXPECT_TRUE(Search("bc", "abcd"));  // regex_search, not regex_match
  EXPECT_FALSE(Search("bc", "b c"));
  EXPECT_TRUE(Search("", "anything"));  // empty pattern matches everywhere
  EXPECT_TRUE(Search("a", "a"));
  EXPECT_FALSE(Search("a", ""));
}

TEST(RegexTest, AnchorsPinTheMatch) {
  EXPECT_TRUE(Search("^ab", "abc"));
  EXPECT_FALSE(Search("^bc", "abc"));
  EXPECT_TRUE(Search("bc$", "abc"));
  EXPECT_FALSE(Search("ab$", "abc"));
  EXPECT_TRUE(Search("^abc$", "abc"));
  EXPECT_FALSE(Search("^abc$", "abcd"));
  EXPECT_TRUE(Search("^$", ""));
  EXPECT_FALSE(Search("^$", "x"));
}

TEST(RegexTest, TheFixturePatterns) {
  // Every @pattern in the checked-in fixture models.
  EXPECT_TRUE(Search("^[A-Za-z0-9 ]+$", "Seattle 98101"));
  EXPECT_FALSE(Search("^[A-Za-z0-9 ]+$", "nope!"));
  EXPECT_FALSE(Search("^[A-Za-z0-9 ]+$", ""));
}

TEST(RegexTest, ClassesRangesAndNegation) {
  EXPECT_TRUE(Search("[a-c]", "b"));
  EXPECT_FALSE(Search("[a-c]", "d"));
  EXPECT_TRUE(Search("[^a-c]", "d"));
  EXPECT_FALSE(Search("^[^a-c]+$", "abc"));
  EXPECT_TRUE(Search("[]a]", "]"));  // ']' first in a class is a literal
  EXPECT_TRUE(Search("[a-]", "-"));  // trailing '-' is a literal
  EXPECT_TRUE(Search("[-a]", "-"));
  EXPECT_TRUE(Search("[\\d]", "7"));
  EXPECT_FALSE(Search("^[\\d-]+$", "7x"));
}

TEST(RegexTest, ClassEscapes) {
  EXPECT_TRUE(Search("^\\d+$", "0123456789"));
  EXPECT_FALSE(Search("^\\d+$", "12a"));
  EXPECT_TRUE(Search("^\\w+$", "az_AZ09"));
  EXPECT_FALSE(Search("^\\w+$", "a b"));
  EXPECT_TRUE(Search("^\\s$", "\t"));
  EXPECT_TRUE(Search("^\\S+$", "abc"));
  EXPECT_FALSE(Search("^\\D$", "5"));
  EXPECT_TRUE(Search("^\\W$", "!"));
}

TEST(RegexTest, QuantifiersIncludingCounted) {
  EXPECT_TRUE(Search("^a*$", ""));
  EXPECT_TRUE(Search("^a+$", "aaa"));
  EXPECT_FALSE(Search("^a+$", ""));
  EXPECT_TRUE(Search("^ab?c$", "ac"));
  EXPECT_TRUE(Search("^a{3}$", "aaa"));
  EXPECT_FALSE(Search("^a{3}$", "aa"));
  EXPECT_TRUE(Search("^a{2,}$", "aaaa"));
  EXPECT_FALSE(Search("^a{2,}$", "a"));
  EXPECT_TRUE(Search("^a{1,3}$", "aa"));
  EXPECT_FALSE(Search("^a{1,3}$", "aaaa"));
  EXPECT_TRUE(Search("^a*?b$", "aab"));   // lazy quantifiers accepted
  EXPECT_TRUE(Search("a{,3}", "a{,3}"));  // not a quantifier: literal '{'
}

TEST(RegexTest, GroupsAndAlternation) {
  EXPECT_TRUE(Search("^(ab)+$", "ababab"));
  EXPECT_FALSE(Search("^(ab)+$", "aba"));
  EXPECT_TRUE(Search("^(a|bc)d$", "bcd"));
  EXPECT_TRUE(Search("^(?:xy){2}$", "xyxy"));
  EXPECT_TRUE(Search("cat|dog", "hotdog"));
  EXPECT_FALSE(Search("^(cat|dog)$", "cow"));
  EXPECT_TRUE(Search("^(?<name>ab)$", "ab"));  // named group = plain group
}

TEST(RegexTest, DotAndEscapedLiterals) {
  EXPECT_TRUE(Search("^a.c$", "abc"));
  EXPECT_FALSE(Search("^a.c$", "a\nc"));
  EXPECT_TRUE(Search("^a\\.c$", "a.c"));
  EXPECT_FALSE(Search("^a\\.c$", "abc"));
  EXPECT_TRUE(Search("^\\x41$", "A"));
  EXPECT_TRUE(Search("^\\u0041$", "A"));
  EXPECT_TRUE(Search("^\\u00e9$", "\xc3\xa9"));  // é as UTF-8 bytes
  EXPECT_TRUE(Search("\\$\\^\\(\\)\\[\\]", "$^()[]"));
}

TEST(RegexTest, WordBoundaries) {
  EXPECT_TRUE(Search("\\bcat\\b", "a cat sat"));
  EXPECT_FALSE(Search("\\bcat\\b", "concatenate"));
  EXPECT_TRUE(Search("\\Bcat\\B", "concatenate"));
  EXPECT_FALSE(Search("\\Bcat\\B", "a cat sat"));
}

TEST(RegexTest, UnsupportedConstructsFailAtCompileTime) {
  EXPECT_FALSE(Compiles("(a)\\1"));   // backreference
  EXPECT_FALSE(Compiles("a(?=b)"));   // lookahead
  EXPECT_FALSE(Compiles("a(?!b)"));   // negative lookahead
  EXPECT_FALSE(Compiles("(?<=a)b"));  // lookbehind
  EXPECT_FALSE(Compiles("(?<!a)b"));  // negative lookbehind
  EXPECT_FALSE(Compiles("(ab"));      // unbalanced
  EXPECT_FALSE(Compiles("ab)"));
  EXPECT_FALSE(Compiles("[ab"));
  EXPECT_FALSE(Compiles("*a"));
  EXPECT_FALSE(Compiles("[z-a]"));
  EXPECT_FALSE(Compiles("(a{1000}){1000}"));  // program-size cap
  EXPECT_TRUE(Compiles("a{99999}"));          // over the repeat cap: literal '{' (Annex B)
  EXPECT_TRUE(Search("^a{99999}$", "a{99999}"));
}

// The whole point: the deliberately catastrophic pattern from the protocol
// test suites evaluates in linear time. Under a backtracking engine this
// input explores ~2^100000 paths; the Pike VM's stamp dedup runs each
// instruction at most once per input position, so the step count is bounded
// by program size x (input size + 2). Asserting on the counter keeps the
// bound deterministic — a loaded CI runner can't flake it the way a
// wall-clock limit could.
TEST(RegexTest, CatastrophicPatternIsLinear) {
  auto re = opal::Regex::Compile("^([0-9]+)+$");
  ASSERT_TRUE(re.ok());
  std::string evil(100000, '1');
  evil.push_back('!');
  std::size_t steps = 0;
  EXPECT_FALSE(re->Search(evil, &steps));
  EXPECT_LE(steps, re->ProgramSize() * (evil.size() + 2));
  std::string good(100000, '7');
  EXPECT_TRUE(re->Search(good));
}

TEST(RegexTest, MoreNestedQuantifierBombs) {
  std::string as(50000, 'a');
  const std::string bomb = as + "b";
  for (const char* pattern : {"^(a+)+$", "^(a|a)+$", "^(a*)*$"}) {
    auto re = opal::Regex::Compile(pattern);
    ASSERT_TRUE(re.ok()) << pattern;
    std::size_t steps = 0;
    EXPECT_FALSE(re->Search(bomb, &steps)) << pattern;
    EXPECT_LE(steps, re->ProgramSize() * (bomb.size() + 2)) << pattern;
  }
  EXPECT_TRUE(Search("^(a+)+$", as));
}

// Differential check against std::regex: on patterns std::regex handles
// without pathological backtracking, both engines must agree.
TEST(RegexTest, AgreesWithStdRegexOnRandomInputs) {
  const char* patterns[] = {
      "^[A-Za-z0-9 ]+$", "^\\d{1,3}(\\.\\d{1,3}){3}$",
      "^(ab|cd)*e?$",    "[a-f]+\\d*",
      "^x.y$",           "\\bword\\b",
      "^-?\\d+$",        "^[^0-9]*$",
  };
  std::mt19937 rng(20260708);
  std::uniform_int_distribution<int> len(0, 12);
  const std::string alphabet = "abcdexy01239. -_";
  std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
  for (const char* pattern : patterns) {
    auto mine = opal::Regex::Compile(pattern);
    ASSERT_TRUE(mine.ok()) << pattern;
    const std::regex theirs(pattern, std::regex::ECMAScript);
    for (int i = 0; i < 500; ++i) {
      std::string text;
      const int n = len(rng);
      for (int j = 0; j < n; ++j) text.push_back(alphabet[pick(rng)]);
      EXPECT_EQ(mine->Search(text), std::regex_search(text, theirs))
          << "pattern: " << pattern << " text: '" << text << "'";
    }
  }
}

}  // namespace
