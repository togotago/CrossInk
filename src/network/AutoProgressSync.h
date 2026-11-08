#pragma once

#include <cmath>
#include <cstdint>

// Pushes the reading position of every book with a pending progress change when
// the reader sleeps. Called from enterDeepSleep() after the sleep screen is on
// the panel, so the burst is invisible to the user and runs with the reader's
// memory already released. It also runs for the idle-timeout sleep: nothing is
// sent unless a book is pending, and a change that is not pushed now would only
// be pushed at the next sleep or manual sync.
//
// The server is shared with other readers (a KOReader partner pulls the newest
// position), so a position behind the server is never uploaded: it is parked
// instead and offered on the book's next open. A failed book keeps its pending
// bit and is retried at the next sleep or manual sync.
//
// The radio is joined lazily, per book: the saved position and the parked
// remote are compared locally first, so a book whose remote is already ahead
// costs no Wi-Fi join on any sleep. Only a book that actually needs a GET or a
// PUT brings the radio up, and it is switched off again before returning.
namespace AutoProgressSync {

// Whole-burst cap. It only delays power-off; the panel already shows the sleep
// screen.
constexpr uint32_t BURST_BUDGET_MS = 8000;
// Association plus DHCP for the last saved network.
constexpr uint32_t CONNECT_BUDGET_MS = 5000;
// Below this the burst is skipped; the same floor the plugin sleep hook uses.
constexpr uint8_t MIN_BATTERY_PERCENT = 20;
// A sync boot that never reaches deep sleep must not loop. The counter is RTC
// memory: after this many back-to-back sync boots the device stops arming them
// and takes the ordinary sleep path instead.
constexpr uint32_t MAX_SYNC_BOOT_ATTEMPTS = 2;
// 0.1 percentage points, the same epsilon Smart sync uses.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

// Whether sleep should hand the pending upload to the headless sync boot.
// The burst cannot fit in the reader's process (Wi-Fi leaves ~38 KB free and
// ~33 KB max block even after the reader releases everything), so it runs in
// the same minimal boot every other network feature uses. Pure so the gate is
// tested on its own; pushPending() re-checks the same conditions when it runs.
inline bool shouldArmSyncBoot(const bool autoSyncOn, const bool hasCredentials, const uint8_t batteryPercent,
                              const uint32_t progressPending, const uint32_t attempts) {
  return autoSyncOn && hasCredentials && batteryPercent >= MIN_BATTERY_PERCENT && progressPending > 0 &&
         attempts < MAX_SYNC_BOOT_ATTEMPTS;
}

enum class Decision : uint8_t {
  Upload,       // local is ahead, or the server has no position for the book
  RemoteAhead,  // keep the local change pending and park the remote position
  InSync,       // within the epsilon: the server already has this position
};

// Smart-sync decision for one book. Pure, so the rule is tested on its own.
inline Decision decide(const float localPercentage, const bool hasRemote, const float remotePercentage) {
  if (!hasRemote) return Decision::Upload;
  const float delta = localPercentage - remotePercentage;
  if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) return Decision::InSync;
  return delta > 0 ? Decision::Upload : Decision::RemoteAhead;
}

// Wraps with the same uint32 millis() arithmetic as the caller's clock.
inline bool budgetExhausted(const uint32_t startMs, const uint32_t nowMs, const uint32_t budgetMs = BURST_BUDGET_MS) {
  return static_cast<uint32_t>(nowMs - startMs) >= budgetMs;
}

// What the burst does with one book. The pending bit is cleared whenever the
// decision settles the book without a successful upload: the server already
// holds this position, or it holds a newer one. A book behind the server would
// never be uploaded under the Smart rule, so keeping it pending would only
// retry that comparison at every sleep; the next progress save re-marks it.
// A book that could not be handled at all simply keeps its bit (no action).
enum class PendingAction : uint8_t {
  UploadAndClearOnSuccess,
  ClearNow,  // the server holds this position or a newer one
};

inline PendingAction pendingActionFor(const Decision decision) {
  return decision == Decision::Upload ? PendingAction::UploadAndClearOnSuccess : PendingAction::ClearNow;
}

// wl_status_t values from Arduino's WiFiType.h, repeated so the rule below can
// be tested on the host, whose build has no Arduino headers. AutoProgressSync.cpp
// static-asserts them against the real enum.
constexpr int WL_STATUS_NO_SSID_AVAIL = 1;
constexpr int WL_STATUS_CONNECTED = 3;
constexpr int WL_STATUS_CONNECT_FAILED = 4;

enum class JoinSignal : uint8_t {
  Waiting,      // starting, associating, or waiting for DHCP
  Connected,    // associated and has an address
  NoApInRange,  // the driver's scan found no access point for this SSID
  Refused,      // the access point answered and refused, or association failed
};

// A join is one attempt with one budget: a remembered channel is only a
// scan-start hint, so there is nothing to retry differently. These two statuses
// are terminal and end the attempt at once instead of waiting out the budget:
// the driver sets them after its own scan/association answer, so "out and
// about" costs a scan (~1-1.5 s), not the full budget.
inline JoinSignal joinSignal(const int wlStatus) {
  switch (wlStatus) {
    case WL_STATUS_CONNECTED:
      return JoinSignal::Connected;
    case WL_STATUS_NO_SSID_AVAIL:
      return JoinSignal::NoApInRange;
    case WL_STATUS_CONNECT_FAILED:
      return JoinSignal::Refused;
    default:
      return JoinSignal::Waiting;
  }
}

struct Result {
  enum class Status : uint8_t {
    Skipped,    // setting off, no credentials, nothing pending, or battery low
    NoNetwork,  // a request was needed and the saved network did not come up
    Ran,        // the burst completed, with or without a network
  };
  Status status = Status::Skipped;
  uint8_t pushed = 0;  // books uploaded and confirmed
  uint8_t parked = 0;  // remote ahead: parked for the next open, bit cleared
  uint8_t inSync = 0;  // already on the server; pending bit cleared
  uint8_t failed = 0;  // kept pending for the next sleep or manual sync
  bool budgetHit = false;
};

// Adds free heap and the largest free block to the SD trace of the next burst.
void noteHeap(const char* stage);

// One extra SD trace line for the headless sync boot: total milliseconds from
// the restart to the start of deep sleep, so the cost of that boot is visible
// and comparable across devices.
void noteSyncBootTotal(uint32_t totalMs);

// Manual-sync timing trace: one "boot=manual" line in the same SD trace the
// sleep burst writes, started at activity entry and flushed before the return
// reboot. Each mark records boot-relative millis() and the line carries the
// trusted epoch second, so the two boots of a manual sync line up. Marks are
// dropped until start, so bulk and folder syncs never reach this file.
void traceManualStart(uint32_t atMs);
void traceManualMark(const char* phase, uint32_t atMs);
void traceManualFlush();

// Whether sleep should restart into the headless sync boot. Reads the same
// stores pushPending() gates on, so a device with the setting off, no
// credentials, a low battery or nothing pending never pays a boot.
bool wantsSleepSync(uint8_t batteryPercent, uint32_t attempts);

// Joins the last saved network with one WiFi.begin() and one overall budget,
// ending at once when the driver reports no AP in range or a refusal. Shared by
// the sleep burst and the manual sync's quick connect. Leaves the radio up on
// success and in station mode on failure; the caller owns both.
bool joinKnownNetwork(uint32_t budgetMs);

// batteryPercent is the same reading the plugin sleep hook uses (main.cpp).
// syncBoot marks the SD trace line so a burst's origin is unambiguous.
// Leaves Wi-Fi off.
Result pushPending(uint8_t batteryPercent, bool syncBoot = false);

}  // namespace AutoProgressSync
