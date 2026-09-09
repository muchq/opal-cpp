// The generated-union accessor contract (issue #49): wrong-case as_x() dies
// naming the union, the requested member, and the engaged member — never a
// context-free std::bad_variant_access — and the checked alternatives
// (is_x/as_x_or_null/case_name/visit) never die. Uses the cafe example's
// OrderStatus, the golden three-member union.

#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "example/cafe/types.h"
#include "opal/core/overloaded.h"

namespace {

using example::cafe::CancelledStatus;
using example::cafe::OrderStatus;
using example::cafe::PendingStatus;
using example::cafe::ReadyStatus;

TEST(UnionAccessorTest, RightCaseAccessReturnsTheMember) {
  const OrderStatus status = OrderStatus::FromPending(PendingStatus{.position = 3});
  ASSERT_TRUE(status.is_pending());
  EXPECT_EQ(status.as_pending().position, 3);
}

TEST(UnionAccessorTest, PointerAccessorReturnsEngagedMemberOrNull) {
  const OrderStatus status = OrderStatus::FromPending(PendingStatus{.position = 3});
  const PendingStatus* pending = status.as_pending_or_null();
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->position, 3);
  EXPECT_EQ(status.as_ready_or_null(), nullptr);
  EXPECT_EQ(status.as_cancelled_or_null(), nullptr);
  EXPECT_EQ(OrderStatus{}.as_pending_or_null(), nullptr);
}

TEST(UnionAccessorTest, CaseNameTracksTheEngagedMember) {
  EXPECT_STREQ(OrderStatus{}.case_name(), "(empty)");
  EXPECT_STREQ(OrderStatus::FromPending(PendingStatus{}).case_name(), "pending");
  EXPECT_STREQ(OrderStatus::FromCancelled(CancelledStatus{}).case_name(), "cancelled");
}

TEST(UnionAccessorTest, VisitDispatchesOverMembersAndEmptyState) {
  const auto describe = opal::Overloaded{
      [](const PendingStatus& p) { return "pending:" + std::to_string(p.position); },
      [](const ReadyStatus&) { return std::string("ready"); },
      [](const CancelledStatus&) { return std::string("cancelled"); },
      [](std::monostate) { return std::string("empty"); },
  };
  EXPECT_EQ(OrderStatus::FromPending(PendingStatus{.position = 7}).visit(describe), "pending:7");
  EXPECT_EQ(OrderStatus{}.visit(describe), "empty");
}

TEST(UnionAccessorDeathTest, WrongCaseDiesNamingRequestedAndEngagedMembers) {
  const OrderStatus status = OrderStatus::FromPending(PendingStatus{.position = 3});
  EXPECT_DEATH((void)status.as_ready(), "OrderStatus::as_ready\\(\\): engaged member is pending");
}

TEST(UnionAccessorDeathTest, EmptyUnionAccessDiesNamingTheEmptyState) {
  const OrderStatus status;
  EXPECT_DEATH((void)status.as_cancelled(),
               "OrderStatus::as_cancelled\\(\\): engaged member is \\(empty\\)");
}

}  // namespace
