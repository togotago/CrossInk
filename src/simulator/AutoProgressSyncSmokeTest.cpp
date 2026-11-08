#ifdef SIMULATOR
#include "AutoProgressSyncSmokeTest.h"

#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "WifiCredentialStore.h"
#include "network/AutoProgressSync.h"
#include "sync/SyncParked.h"
#include "sync/SyncPending.h"

namespace {
void require(const bool ok, const char* what) {
  if (!ok) {
    LOG_ERR("SMOKE", "Auto progress sync contract failed: %s", what);
    std::_Exit(2);
  }
}

constexpr char NETWORK[] = "autosync-smoke";
constexpr char FIRST[] = "/read/first.epub";
constexpr char SECOND[] = "/read/sub/second.EPUB";

// True while the book still has its pending progress bit.
bool progressPending(const char* path) {
  SyncPending::Cursor cursor;
  if (!cursor.begin()) return false;
  std::string entry;
  uint8_t mask = 0;
  while (cursor.next(entry, mask) == SyncPending::Cursor::Step::Entry) {
    if (entry == path) return (mask & SyncPending::PROGRESS_BIT) != 0;
  }
  return false;
}
}  // namespace

void verifyAutoProgressSyncContract() {
  const char* server = std::getenv("CROSSINK_AUTOSYNC_TEST_SERVER");
  if (!server) return;
  const char* mode = std::getenv("CROSSINK_AUTOSYNC_TEST_MODE");
  if (!mode) return;

  KOREADER_STORE.setCredentials("smoke", "smoke-password");
  KOREADER_STORE.setServerUrl(server);
  // The batch route only exists on a CrossPoint Sync profile.
  KOREADER_STORE.setServerType(KOReaderServerType::CROSSPOINT);
  KOREADER_STORE.setAutoSyncProgress(true);
  require(WIFI_STORE.addCredential(NETWORK, "smoke-password"), "saved Wi-Fi network");
  WIFI_STORE.setLastConnectedSsid(NETWORK);

  SyncParked::clear();
  require(Storage.exists(FIRST) && Storage.exists(SECOND), "reading fixtures present");

  const bool batch = strcmp(mode, "batch") == 0;
  require(SyncPending::mark(FIRST, SyncPending::PROGRESS_BIT), "first book pending");
  if (batch) require(SyncPending::mark(SECOND, SyncPending::PROGRESS_BIT), "second book pending");

  const auto result = AutoProgressSync::pushPending(100);

  if (strcmp(mode, "no-network") == 0) {
    require(result.status == AutoProgressSync::Result::Status::NoNetwork, "reports no network");
    require(result.pushed == 0 && progressPending(FIRST), "keeps the pending bit without a network");
  } else if (strcmp(mode, "remote-ahead") == 0) {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    require(result.pushed == 0, "does not upload a position behind the server");
    require(result.parked == 1 && !SyncParked::empty(), "parks the further-ahead position");
    require(progressPending(FIRST), "keeps the local change pending");
  } else if (strcmp(mode, "server-error") == 0) {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    require(result.pushed == 0 && result.failed == 1, "reports the failed upload");
    require(progressPending(FIRST), "keeps the pending bit after a 500");
  } else if (batch) {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    require(result.pushed == 2, "uploads both books in one batch");
    require(!progressPending(FIRST) && !progressPending(SECOND), "clears both confirmed bits");
  } else {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    require(result.pushed == 1 && result.failed == 0, "uploads the pending book");
    require(!progressPending(FIRST), "clears the confirmed pending bit");
  }

  LOG_INF("SMOKE", "Auto progress sync transport smoke passed (%s)", mode);
  std::_Exit(0);
}
#endif  // SIMULATOR
