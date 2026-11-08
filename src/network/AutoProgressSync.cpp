#include "AutoProgressSync.h"

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <string>
#include <vector>

#include "KOReaderCredentialStore.h"
#include "ReadingSyncUpload.h"
#include "WifiCredentialStore.h"
#include "WifiPowerSaveGuard.h"
#include "WifiUtils.h"
#include "sync/SyncParked.h"
#include "sync/SyncPending.h"

namespace {

constexpr char TAG[] = "AutoSync";

// The X4 may have no usable serial port (USB-locked units), so each burst
// also leaves one line on the SD card: the same messages, joined, plus the
// summary. Capped so a forgotten toggle cannot fill a card.
constexpr char TRACE_PATH[] = "/.crosspoint/autosync.log";
constexpr char TRACE_OLD_PATH[] = "/.crosspoint/autosync.old.log";
constexpr size_t TRACE_MAX_BYTES = 32u * 1024u;
char trace[640];
size_t traceLen = 0;

void traceAppend(const char* format, ...) {
  if (traceLen >= sizeof(trace) - 1) return;
  if (traceLen > 0) trace[traceLen++] = '|';
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(trace + traceLen, sizeof(trace) - traceLen, format, args);
  va_end(args);
  if (written > 0) traceLen = std::min(sizeof(trace) - 1, traceLen + static_cast<size_t>(written));
  trace[traceLen] = '\0';
}

void traceFlush() {
  if (traceLen == 0) return;
  HalFile probe = Storage.open(TRACE_PATH, O_RDONLY);
  const size_t existing = probe ? probe.size() : 0;
  if (probe) probe.close();
  if (existing > TRACE_MAX_BYTES) {
    Storage.remove(TRACE_OLD_PATH);
    Storage.rename(TRACE_PATH, TRACE_OLD_PATH);
  }
  HalFile file = Storage.open(TRACE_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (file) {
    char stamp[32];
    const int64_t now = trustedtime::trustedNow();
    const int stampLen = now > 0 ? snprintf(stamp, sizeof(stamp), "%lld ", static_cast<long long>(now))
                                 : snprintf(stamp, sizeof(stamp), "untrusted-clock ");
    file.write(stamp, static_cast<size_t>(stampLen));
    file.write(trace, traceLen);
    file.write("\n", 1);
    file.close();  // close() syncs; the SD rail is cut right after this burst
  }
  traceLen = 0;
  trace[0] = '\0';
}

// Serial log and SD trace in one call.
#define ALOG(level, format, ...)                     \
  do {                                               \
    level(TAG, format, ##__VA_ARGS__);               \
    traceAppend(format, ##__VA_ARGS__);              \
  } while (0)
// Bounded clock work: an untrusted clock only skews event timestamps, and the
// upload carries no timestamp of its own, so this is never worth the 5 s the
// manual sync path allows.
constexpr uint32_t NTP_BUDGET_MS = 2000;

bool connectLastKnownNetwork(const uint32_t budgetMs) {
  if (hasActiveStationWifiConnection()) return true;
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential) {
    ALOG(LOG_INF, "No saved network for the last SSID");
    return false;
  }
  WiFi.mode(WIFI_STA);
  const uint32_t start = millis();

#ifndef SIMULATOR
  // Remembering the channel and BSSID skips a full-channel scan (measured
  // 798 ms -> 205 ms for association). The hint is only for this network, and
  // a stale one costs its 2 s budget, never the whole connect.
  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  const bool hasCachedAp = WIFI_STORE.getLastAp(credential->ssid, bssid, channel);
  if (AutoProgressSync::joinPhase(hasCachedAp, 0) == AutoProgressSync::JoinPhase::CachedAp) {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str(), channel, bssid);
    while (WiFi.status() != WL_CONNECTED && millis() - start < AutoProgressSync::FAST_JOIN_BUDGET_MS) delay(50);
    if (WiFi.status() == WL_CONNECTED) {
      ALOG(LOG_INF, "WiFi joined the cached AP in %u ms", static_cast<unsigned>(millis() - start));
      return true;
    }
    ALOG(LOG_INF, "Cached AP did not answer; falling back to a normal join");
    WiFi.disconnect(false, false, 1000);
  }
#endif

  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= budgetMs) {
      ALOG(LOG_INF, "WiFi join timed out after %u ms", static_cast<unsigned>(budgetMs));
      return false;
    }
    delay(50);
  }
  ALOG(LOG_INF, "WiFi joined in %u ms", static_cast<unsigned>(millis() - start));
#ifndef SIMULATOR
  // Learn the access point for the next burst, whatever path joined it.
  uint8_t joinedBssid[6] = {};
  const int joinedChannel = WiFi.channel();
  if (WiFi.BSSID(joinedBssid) && joinedChannel > 0) {
    WIFI_STORE.setLastAp(credential->ssid, joinedBssid, static_cast<uint8_t>(joinedChannel));
  }
#endif
  return true;
}

void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// Keep the newest remote position this reader saw for a book it cannot apply:
// the next open offers it through KeptPosition, as the bulk pull does.
void parkRemote(const KOReaderProgress& remote) {
  KOReaderProgressChange change;
  change.document = remote.document;
  change.percentage = remote.percentage;
  change.progress = remote.progress;
  change.position = remote.position;
  change.device = remote.device;
  change.timestamp = remote.timestamp;
  if (!SyncParked::park(change)) ALOG(LOG_ERR, "Could not park the remote position");
}

// Sends the collected positions: several books go in one batch request (one TLS
// handshake), and a server without that route gets one PUT per book instead, as
// the manual bulk sync does. A single book is always a plain PUT.
// Only confirmed (2xx) books lose their pending bit. All requests of one burst
// share one TLS connection.
void flush(AutoProgressSync::Result& result, std::vector<KOReaderProgress>& batch, std::vector<std::string>& paths,
           KOReaderSyncClient::Connection& connection) {
  if (batch.empty()) return;
  bool oneByOne = batch.size() == 1;
  if (!oneByOne) {
    const auto error = KOReaderSyncClient::updateProgressBatch(batch, &connection);
    // Consumed either way: never reused after the request.
    batch.clear();
    if (error == KOReaderSyncClient::BATCH_UNSUPPORTED) {
      // Stock KOSync and older CrossPoint Sync servers: fall back per book.
      // The positions were released with the batch, so they are mapped again;
      // this only happens on a server without the route.
      ALOG(LOG_INF, "No batch progress route; sending one book at a time");
      oneByOne = true;
    } else if (error != KOReaderSyncClient::OK) {
      ALOG(LOG_INF, "Batch PUT failed (%s); keeping them pending", KOReaderSyncClient::errorString(error).c_str());
      result.failed += static_cast<uint8_t>(paths.size());
      paths.clear();
      return;
    }
  }
  if (oneByOne) {
    for (size_t i = 0; i < paths.size(); ++i) {
      KOReaderProgress progress;
      if (i < batch.size()) {
        progress = std::move(batch[i]);
      } else if (!ReadingSyncUpload::prepareProgress(paths[i], progress)) {
        ALOG(LOG_INF, "No usable saved position on retry; keeping it pending: %s", paths[i].c_str());
        ++result.failed;
        continue;
      }
      const auto error = KOReaderSyncClient::updateProgress(progress, &connection);
      if (error != KOReaderSyncClient::OK) {
        ALOG(LOG_INF, "PUT failed (%s); keeping it pending", KOReaderSyncClient::errorString(error).c_str());
        ++result.failed;
        continue;
      }
      SyncPending::clear(paths[i], SyncPending::PROGRESS_BIT);
      ++result.pushed;
    }
    batch.clear();
    paths.clear();
    return;
  }
  for (const auto& path : paths) {
    SyncPending::clear(path, SyncPending::PROGRESS_BIT);
    ++result.pushed;
  }
  paths.clear();
}

AutoProgressSync::Result runBurst() {
  AutoProgressSync::Result result;
  // Modem power save during TLS is a known cause of intermittent handshake
  // failures, so it is switched off for the burst. The guard is only armed when
  // a request is actually needed; the destructor restores the previous mode.
  WifiPowerSaveGuard powerSave;
  const uint32_t start = millis();
  SyncPending::Cursor cursor;
  if (!cursor.begin()) {
    ALOG(LOG_ERR, "Pending list unreadable; skipping");
    return result;
  }

  std::vector<KOReaderProgress> batch;
  std::vector<std::string> batchPaths;
  batch.reserve(KOReaderSyncClient::MAX_PROGRESS_BATCH);
  batchPaths.reserve(KOReaderSyncClient::MAX_PROGRESS_BATCH);
  // One TLS session for the whole burst: the GET of each book and the PUT or
  // batch PUT share it instead of paying a handshake per request.
  KOReaderSyncClient::Connection connection;

  // The radio is joined at most once per burst, and only when a GET or PUT is
  // really needed: a book whose parked remote is already ahead is settled from
  // the SD card alone, so it must never cost a Wi-Fi join.
  enum class Network : uint8_t { NotTried, Up, Failed };
  Network network = Network::NotTried;
  const auto ensureNetwork = [&]() {
    if (network == Network::Up) return true;
    if (network == Network::Failed) return false;  // one attempt per burst
    powerSave.arm();
    if (!connectLastKnownNetwork(AutoProgressSync::CONNECT_BUDGET_MS)) {
      network = Network::Failed;
      result.status = AutoProgressSync::Result::Status::NoNetwork;
      return false;
    }
    if (trustedtime::trustedNow() == 0) trustedtime::syncNow(NTP_BUDGET_MS);
    network = Network::Up;
    return true;
  };

  std::string path;
  uint8_t mask = 0;
  bool handled = false;
  while (cursor.next(path, mask) == SyncPending::Cursor::Step::Entry) {
    if (!(mask & SyncPending::PROGRESS_BIT) || !FsHelpers::hasEpubExtension(path)) continue;
    if (AutoProgressSync::budgetExhausted(start, millis())) {
      result.budgetHit = true;
      break;
    }
    if (!Storage.exists(path.c_str())) {
      // Moved or deleted outside the reader: nothing to map. Keep the bit.
      ALOG(LOG_INF, "Pending book missing; keeping it pending: %s", path.c_str());
      ++result.failed;
      continue;
    }

    KOReaderProgress local;
    if (!ReadingSyncUpload::prepareProgress(path, local)) {
      // No usable saved position (unread, or failed to map): keep the bit, as
      // the manual sync does.
      ALOG(LOG_INF, "No usable saved position; keeping it pending: %s", path.c_str());
      ++result.failed;
      continue;
    }

    SyncParked::Entry parked;
    KOReaderProgress remote;
    bool hasRemote = false;
    bool fromParked = false;
    if (SyncParked::find(local.document, parked)) {
      // The parked position is the newest this reader saw; compare it instead
      // of a GET so it is never overwritten blindly, and without a radio.
      remote = parked.toProgress(local.document);
      hasRemote = true;
      fromParked = true;
      ALOG(LOG_INF, "Comparing with a parked remote position: %s", path.c_str());
    } else {
      if (AutoProgressSync::budgetExhausted(start, millis())) {
        result.budgetHit = true;
        break;
      }
      if (!ensureNetwork()) {
        ++result.failed;
        continue;
      }
      const auto error = KOReaderSyncClient::getProgress(local.document, remote, &connection);
      if (error == KOReaderSyncClient::OK) {
        hasRemote = true;
      } else if (error != KOReaderSyncClient::NOT_FOUND) {
        ALOG(LOG_INF, "GET failed (%s); keeping it pending", KOReaderSyncClient::errorString(error).c_str());
        ++result.failed;
        // A dead transport or exhausted heap fails every remaining book too.
        if (error == KOReaderSyncClient::NETWORK_ERROR || error == KOReaderSyncClient::LOW_MEMORY) break;
        continue;
      }
    }

    const auto decision = AutoProgressSync::decide(local.percentage, hasRemote, remote.percentage);
    handled = true;
    switch (AutoProgressSync::pendingActionFor(decision)) {
      case AutoProgressSync::PendingAction::UploadAndClearOnSuccess:
        if (!ensureNetwork()) {
          ++result.failed;
          break;
        }
        batchPaths.push_back(path);
        batch.push_back(std::move(local));
        // The batch route takes at most eight positions; flush a full batch
        // instead of growing it past what one request can send.
        if (batch.size() >= KOReaderSyncClient::MAX_PROGRESS_BATCH) flush(result, batch, batchPaths, connection);
        break;
      case AutoProgressSync::PendingAction::ClearNow:
        if (decision == AutoProgressSync::Decision::RemoteAhead) {
          // The partner device is further ahead. Never move the server
          // backwards; offer the remote position on the book's next open and
          // drop the pending bit, because this change can never be uploaded
          // under the Smart rule. The next progress save re-marks the book.
          if (hasRemote && !fromParked) parkRemote(remote);
          ++result.parked;
        } else {
          ++result.inSync;
        }
        SyncPending::clear(path, SyncPending::PROGRESS_BIT);
        break;
    }
  }
  cursor.end();

  flush(result, batch, batchPaths, connection);
  if (network == Network::Up) wifiOff();
  // Books settled from the SD card run no request, but the burst did run.
  if (result.status == AutoProgressSync::Result::Status::Skipped && handled) {
    result.status = AutoProgressSync::Result::Status::Ran;
  }
  return result;
}

}  // namespace

AutoProgressSync::Result AutoProgressSync::pushPending(const uint8_t batteryPercent) {
  Result result;
  if (!KOREADER_STORE.getAutoSyncProgress()) return result;
  traceLen = 0;
  traceAppend("battery=%u%%", static_cast<unsigned>(batteryPercent));
  if (!KOREADER_STORE.hasCredentials()) {
    ALOG(LOG_DBG, "No sync credentials; skipping");
    traceFlush();
    return result;
  }
  if (batteryPercent < MIN_BATTERY_PERCENT) {
    ALOG(LOG_INF, "Battery %u%% is below the sync floor; keeping positions pending",
         static_cast<unsigned>(batteryPercent));
    traceFlush();
    return result;
  }
  const uint32_t pending = SyncPending::count();
  if (pending == 0) {
    ALOG(LOG_DBG, "Nothing pending; skipping");
    traceFlush();
    return result;
  }
  traceAppend("pending=%u", static_cast<unsigned>(pending));

  const uint32_t start = millis();
  result = runBurst();
  const char* status = result.status == Result::Status::Ran       ? "ran"
                       : result.status == Result::Status::Skipped ? "skipped"
                                                                  : "no network";
  // One line per sleep, whatever the outcome: this is the only record of what
  // an unattended burst did.
  ALOG(LOG_INF, "Sleep sync %s: pushed=%u parked=%u in-sync=%u failed=%u budget=%s elapsed=%u ms heap=%u maxalloc=%u",
       status, static_cast<unsigned>(result.pushed), static_cast<unsigned>(result.parked),
       static_cast<unsigned>(result.inSync), static_cast<unsigned>(result.failed), result.budgetHit ? "hit" : "ok",
       static_cast<unsigned>(millis() - start), static_cast<unsigned>(ESP.getFreeHeap()),
       static_cast<unsigned>(ESP.getMaxAllocHeap()));
  traceFlush();
  return result;
}
