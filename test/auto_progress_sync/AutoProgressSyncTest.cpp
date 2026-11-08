#include <gtest/gtest.h>

#include <cstdint>

#include "network/AutoProgressSync.h"
#include "network/ReadingSyncUpload.h"

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

// Join fail-fast: one attempt with one budget, and the two statuses the driver
// reports after its own scan/association answer end it at once instead of
// waiting out the budget. Anything else keeps waiting (association, then DHCP).
TEST(AutoProgressSyncJoin, ConnectsOnTheConnectedStatus) {
  EXPECT_EQ(joinSignal(WL_STATUS_CONNECTED), JoinSignal::Connected);
}

TEST(AutoProgressSyncJoin, EndsAtOnceWhenNoAccessPointIsInRange) {
  EXPECT_EQ(joinSignal(WL_STATUS_NO_SSID_AVAIL), JoinSignal::NoApInRange);
}

TEST(AutoProgressSyncJoin, EndsAtOnceWhenTheAccessPointRefuses) {
  EXPECT_EQ(joinSignal(WL_STATUS_CONNECT_FAILED), JoinSignal::Refused);
}

TEST(AutoProgressSyncJoin, KeepsWaitingForEveryOtherStatus) {
  // WL_IDLE_STATUS, WL_SCAN_COMPLETED, WL_CONNECTION_LOST, WL_DISCONNECTED,
  // WL_STOPPED: none of these are the driver's final answer.
  for (const int status : {0, 2, 5, 6, 7}) {
    EXPECT_EQ(joinSignal(status), JoinSignal::Waiting) << status;
  }
}

// Sync-boot gate: the burst runs in its own boot, so a device that would skip
// the upload must never pay that boot. Each condition alone is enough to stop.
TEST(AutoProgressSyncBoot, ArmsOnlyWhenTheBurstWouldRun) {
  EXPECT_TRUE(shouldArmSyncBoot(true, true, MIN_BATTERY_PERCENT, 1, 0));
  EXPECT_TRUE(shouldArmSyncBoot(true, true, 100, 3, MAX_SYNC_BOOT_ATTEMPTS - 1));
}

TEST(AutoProgressSyncBoot, RequiresTheSettingCredentialsBatteryAndWork) {
  EXPECT_FALSE(shouldArmSyncBoot(false, true, 100, 1, 0));  // auto-sync off
  EXPECT_FALSE(shouldArmSyncBoot(true, false, 100, 1, 0));  // no account
  EXPECT_FALSE(shouldArmSyncBoot(true, true, MIN_BATTERY_PERCENT - 1, 1, 0));
  EXPECT_FALSE(shouldArmSyncBoot(true, true, 100, 0, 0));  // nothing pending
}

TEST(AutoProgressSyncBoot, StopsAfterTooManyConsecutiveBoots) {
  EXPECT_FALSE(shouldArmSyncBoot(true, true, 100, 1, MAX_SYNC_BOOT_ATTEMPTS));
  EXPECT_FALSE(shouldArmSyncBoot(true, true, 100, 1, MAX_SYNC_BOOT_ATTEMPTS + 1));
}

// Legacy progress records: older firmware wrote a 6/10-byte record with no page
// count. The manual Sync Book maps it; the sleep sync must accept the same
// record instead of keeping that book pending forever.
TEST(ReadingSyncUploadSavedProgress, RejectsALegacyRecordUnderTheStrictRule) {
  // No page count: pageCount stays 0, which the foreground rule rejects.
  EXPECT_FALSE(ReadingSyncUpload::usableSavedProgress(false, 0, 3, 1, 10, /*tolerateLegacy=*/false));
}

TEST(ReadingSyncUploadSavedProgress, AcceptsALegacyRecordWithTheManualSyncRule) {
  // The tolerant path substitutes a page count of 1 and maps from the page index.
  EXPECT_TRUE(ReadingSyncUpload::usableSavedProgress(false, 1, 3, 1, 10, /*tolerateLegacy=*/true));
}

TEST(ReadingSyncUploadSavedProgress, ClampsAStaleSpineOnlyForTheTolerantPath) {
  EXPECT_FALSE(ReadingSyncUpload::usableSavedProgress(true, 10, 2, 12, 10, /*tolerateLegacy=*/false));
  EXPECT_TRUE(ReadingSyncUpload::usableSavedProgress(true, 10, 2, 0, 10, /*tolerateLegacy=*/true));
}

TEST(ReadingSyncUploadSavedProgress, StillRejectsAnImpossiblePageInBothPaths) {
  // pageNumber >= pageCount is a stale pagination, not a legacy record.
  EXPECT_FALSE(ReadingSyncUpload::usableSavedProgress(true, 10, 10, 1, 10, /*tolerateLegacy=*/false));
  EXPECT_FALSE(ReadingSyncUpload::usableSavedProgress(true, 10, 10, 1, 10, /*tolerateLegacy=*/true));
}

TEST(ReadingSyncUploadSavedProgress, RejectsAnEmptySpineList) {
  EXPECT_FALSE(ReadingSyncUpload::usableSavedProgress(true, 10, 0, 0, 0, /*tolerateLegacy=*/true));
}
