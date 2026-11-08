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
// The cached access point gets this long before a normal join takes over.
constexpr uint32_t FAST_JOIN_BUDGET_MS = 2000;
// Below this the burst is skipped; the same floor the plugin sleep hook uses.
constexpr uint8_t MIN_BATTERY_PERCENT = 20;
// 0.1 percentage points, the same epsilon Smart sync uses.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

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

enum class JoinPhase : uint8_t {
  CachedAp,  // join straight at the remembered channel and BSSID
  FullScan,  // a normal join: the AP roamed, changed channel, or was never seen
};

// A remembered AP is tried first (one scan avoided), but only for its budget:
// mesh and band-steering networks move a client between APs, so the fallback
// must always exist.
inline JoinPhase joinPhase(const bool hasCachedAp, const uint32_t elapsedMs) {
  return hasCachedAp && elapsedMs < FAST_JOIN_BUDGET_MS ? JoinPhase::CachedAp : JoinPhase::FullScan;
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

// batteryPercent is the same reading the plugin sleep hook uses (main.cpp).
// Leaves Wi-Fi off.
Result pushPending(uint8_t batteryPercent);

}  // namespace AutoProgressSync
