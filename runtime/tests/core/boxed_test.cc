// opal::Boxed: value-semantic heap indirection for recursive generated
// members — deep copy, deep equality, and compilability with the mutually
// recursive struct shapes the generator emits.

#include "smithy/core/boxed.h"

#include <gtest/gtest.h>

#include <compare>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opal {
namespace {

// The shape the generator emits for mutual recursion: forward-declared
// targets, Boxed struct members, and a defaulted deep operator==.
struct Nested2;

struct Nested1 {
  std::string foo{};
  std::optional<Boxed<Nested2>> nested{};

  friend bool operator==(const Nested1&, const Nested1&) = default;
};

struct Nested2 {
  std::string bar{};
  std::optional<Boxed<Nested1>> recursiveMember{};

  friend bool operator==(const Nested2&, const Nested2&) = default;
};

// Self-recursion through a list: std::vector permits incomplete elements.
struct TreeNode {
  std::string label{};
  std::vector<TreeNode> children{};

  friend bool operator==(const TreeNode&, const TreeNode&) = default;
};

Nested1 MakeChain() {
  Nested1 root;
  root.foo = "Foo1";
  Nested2 middle;
  middle.bar = "Bar1";
  Nested1 leaf;
  leaf.foo = "Foo2";
  middle.recursiveMember = std::move(leaf);
  root.nested = std::move(middle);
  return root;
}

// Boxed deliberately has no operator<=>: with one, clang hard-errors
// deducing the deep <=>'s return type around the recursive chain Boxed
// exists to break. The generator therefore skips the defaulted <=> for
// recursive structs — they are equality-only — and this pin keeps Boxed
// from ever regaining an ordering.
static_assert(!std::three_way_comparable<Boxed<std::string>>,
              "Boxed is deliberately equality-only; see the comment above");

TEST(BoxedTest, DeepEquality) {
  EXPECT_EQ(MakeChain(), MakeChain());
  Nested1 other = MakeChain();
  (**(**other.nested).recursiveMember).foo = "changed";
  EXPECT_NE(MakeChain(), other);
}

TEST(BoxedTest, CopyIsDeep) {
  Nested1 original = MakeChain();
  Nested1 copy = original;
  (**copy.nested).bar = "changed";
  EXPECT_EQ((**original.nested).bar, "Bar1");
  EXPECT_NE(original, copy);
}

TEST(BoxedTest, AssignFromValueReplacesTheBox) {
  Boxed<Nested2> box;
  EXPECT_EQ(box->bar, "");
  Nested2 value;
  value.bar = "assigned";
  box = std::move(value);
  EXPECT_EQ(box->bar, "assigned");
}

TEST(BoxedTest, ListRecursionComparesStructurally) {
  TreeNode root{.label = "root", .children = {TreeNode{.label = "leaf"}}};
  TreeNode same{.label = "root", .children = {TreeNode{.label = "leaf"}}};
  EXPECT_EQ(root, same);
  same.children[0].label = "other";
  EXPECT_NE(root, same);
}

}  // namespace
}  // namespace opal
