#include <gtest/gtest.h>

#include <cstdint>

#include "network/AutoProgressSync.h"

namespace {
using namespace AutoProgressSync;
}  // namespace

// Smart sync's direction rule: the sleep burst must never move the server
// backwards, because the partner reader takes the newest upload as truth.
TEST(AutoProgressSyncDecision, UploadsWhenTheServerHasNoPosition) {
  EXPECT_EQ(decide(0.0f, false, 0.0f), Decision::Upload);
  EXPECT_EQ(decide(0.5f, false, 0.0f), Decision::Upload);
}

TEST(AutoProgressSyncDecision, UploadsWhenLocalIsFurtherAhead) {
  EXPECT_EQ(decide(0.50f, true, 0.40f), Decision::Upload);
  EXPECT_EQ(decide(0.10f, true, 0.0f), Decision::Upload);
}

TEST(AutoProgressSyncDecision, KeepsTheRemoteWhenAPartnerIsFurtherAhead) {
  EXPECT_EQ(decide(0.40f, true, 0.50f), Decision::RemoteAhead);
  EXPECT_EQ(decide(0.0f, true, 0.01f), Decision::RemoteAhead);
}

TEST(AutoProgressSyncDecision, TreatsPositionsWithinPointOnePercentAsEqual) {
  // 0.001 is the 0.1 percentage-point epsilon Smart sync uses.
  EXPECT_EQ(decide(0.5005f, true, 0.5000f), Decision::InSync);
  EXPECT_EQ(decide(0.5000f, true, 0.5005f), Decision::InSync);
  EXPECT_EQ(decide(0.5010f, true, 0.5000f), Decision::InSync);  // exactly the epsilon
  EXPECT_EQ(decide(0.5011f, true, 0.5000f), Decision::Upload);  // past it
  EXPECT_EQ(decide(0.5000f, true, 0.5011f), Decision::RemoteAhead);
}

// Pending-bit handling: an upload clears on a 2xx, and a decision the server
// has already settled clears at once. A book behind the server is parked for
// its next open and cleared, because the Smart rule would never upload it; the
// next progress save re-marks it.
TEST(AutoProgressSyncPending, ClearsUnlessAnUploadMustSucceed) {
  EXPECT_EQ(pendingActionFor(Decision::Upload), PendingAction::UploadAndClearOnSuccess);
  EXPECT_EQ(pendingActionFor(Decision::InSync), PendingAction::ClearNow);
  EXPECT_EQ(pendingActionFor(Decision::RemoteAhead), PendingAction::ClearNow);
}

TEST(AutoProgressSyncBudget, ExhaustsAtTheCapAndWrapsAround) {
  EXPECT_FALSE(budgetExhausted(1000, 1000, 8000));
  EXPECT_FALSE(budgetExhausted(1000, 8999, 8000));
  EXPECT_TRUE(budgetExhausted(1000, 9000, 8000));
  // millis() wraps; a burst that straddles the wrap still sees its budget.
  const uint32_t start = 0xFFFFFF00u;
  EXPECT_FALSE(budgetExhausted(start, start + 1000u, 8000));
  EXPECT_TRUE(budgetExhausted(start, start + 8000u, 8000));
}

// BSSID cache fall-back: a remembered AP is tried first, then the normal join
// takes over, so a roamed or re-channelled AP can never stop the burst.
TEST(AutoProgressSyncJoin, UsesTheCachedAccessPointOnlyWithinItsBudget) {
  EXPECT_EQ(joinPhase(true, 0), JoinPhase::CachedAp);
  EXPECT_EQ(joinPhase(true, FAST_JOIN_BUDGET_MS - 1), JoinPhase::CachedAp);
  EXPECT_EQ(joinPhase(true, FAST_JOIN_BUDGET_MS), JoinPhase::FullScan);
  EXPECT_EQ(joinPhase(true, FAST_JOIN_BUDGET_MS + 1), JoinPhase::FullScan);
}

TEST(AutoProgressSyncJoin, AlwaysFallsBackWithoutACachedAccessPoint) {
  EXPECT_EQ(joinPhase(false, 0), JoinPhase::FullScan);
  EXPECT_EQ(joinPhase(false, FAST_JOIN_BUDGET_MS - 1), JoinPhase::FullScan);
}
