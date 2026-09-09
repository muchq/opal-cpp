// opal::PageIterator (opal/client/pagination.h): the single-pass range
// adapter generated paginators surface through begin()/end() (issue #49), so
// `for (auto& page : client.PaginateX(input))` works. Iteration yields
// Outcome<Page>&; a failed call is yielded exactly once and then the range
// ends without calling Next() again — mirroring the generated paginator's
// stop-on-first-error contract.

#include "opal/client/pagination.h"

#include <gtest/gtest.h>

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "opal/core/error.h"
#include "opal/core/outcome.h"

namespace opal {
namespace {

// Scripted stand-in for a generated paginator: the same Next() contract,
// plus the Page alias and begin()/end() the generator emits.
struct FakePaginator {
  using Page = std::string;

  std::deque<Outcome<std::optional<std::string>>> script;
  int calls = 0;

  Outcome<std::optional<std::string>> Next() {
    ++calls;
    auto next = std::move(script.front());
    script.pop_front();
    return next;
  }

  PageIterator<FakePaginator> begin() { return PageIterator<FakePaginator>(this); }
  PageIterator<FakePaginator> end() { return {}; }
};

TEST(PageIteratorTest, WalksPagesThenEnds) {
  FakePaginator pages;
  pages.script.emplace_back(std::optional<std::string>("one"));
  pages.script.emplace_back(std::optional<std::string>("two"));
  pages.script.emplace_back(std::optional<std::string>());
  std::vector<std::string> seen;
  for (auto& page : pages) {
    ASSERT_TRUE(page.ok());
    // Note the spelling: `*std::move(page)` would copy (Outcome derefs are
    // lvalue-only); the && overload of value() actually moves.
    seen.push_back(std::move(page).value());
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"one", "two"}));
  EXPECT_EQ(pages.calls, 3);
}

TEST(PageIteratorTest, EmptyPaginationIsAnEmptyRange) {
  FakePaginator pages;
  pages.script.emplace_back(std::optional<std::string>());
  EXPECT_TRUE(pages.begin() == pages.end());
  EXPECT_EQ(pages.calls, 1);
}

TEST(PageIteratorTest, ErrorIsYieldedOnceThenTheRangeEnds) {
  FakePaginator pages;
  pages.script.emplace_back(std::optional<std::string>("one"));
  pages.script.emplace_back(Error::Transport("connection reset"));
  int oks = 0;
  int errors = 0;
  for (auto& page : pages) {
    page.ok() ? ++oks : ++errors;
  }
  EXPECT_EQ(oks, 1);
  EXPECT_EQ(errors, 1);
  // The error ends the range by itself: Next() is never called after it
  // fails (the script holds nothing to answer a third call with).
  EXPECT_EQ(pages.calls, 2);
}

}  // namespace
}  // namespace opal
