#ifdef SIMULATOR
#include "AutoProgressSyncSmokeTest.h"

#include <Epub.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "WifiCredentialStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "network/AutoProgressSync.h"
#include "sync/BookSyncFile.h"
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

// A saved position is what prepareProgress maps, so each book needs one before
// the burst runs. The EPUB is released before the sync owns the card path.
bool saveFixture(const char* path) {
  auto epub = std::make_shared<Epub>(path, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(true, true)) return false;
  return EpubReaderUtils::saveProgress(*epub, 0, 2, 10);
}

// A remote position this reader already saw but could not apply, as the pull
// would have left it.
bool parkAhead(const char* path) {
  KOReaderProgressChange change;
  change.document = BookSyncFile::documentId(path);
  change.percentage = 0.97f;
  change.progress = "/body/DocFragment[2]/body/p[3]/text().0";
  change.device = "Kindle";
  change.timestamp = 1800000000;
  return !change.document.empty() && SyncParked::park(change);
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
  require(saveFixture(FIRST) && saveFixture(SECOND), "saved progress fixtures");

  const bool batch = strcmp(mode, "batch") == 0 || strcmp(mode, "batch-unsupported") == 0;
  const bool parkedAhead = strcmp(mode, "parked-ahead-no-network") == 0;
  require(SyncPending::mark(FIRST, SyncPending::PROGRESS_BIT), "first book pending");
  if (batch) require(SyncPending::mark(SECOND, SyncPending::PROGRESS_BIT), "second book pending");
  if (parkedAhead) require(parkAhead(FIRST), "parked remote fixture");

  const auto result = AutoProgressSync::pushPending(100);

  if (parkedAhead) {
    // The parked remote is ahead, so the book is settled from the card: no
    // join is attempted at all, which is why the unreachable server does not
    // surface as NoNetwork.
    require(result.status == AutoProgressSync::Result::Status::Ran, "settles the parked book without a network");
    require(result.pushed == 0 && result.failed == 0, "sends nothing for a parked book ahead");
    require(result.parked == 1 && !SyncParked::empty(), "keeps the parked position for the next open");
    require(!progressPending(FIRST), "clears the bit of a position that can never be uploaded");
  } else if (strcmp(mode, "no-network") == 0) {
    require(result.status == AutoProgressSync::Result::Status::NoNetwork, "reports no network");
    require(result.pushed == 0 && progressPending(FIRST), "keeps the pending bit without a network");
  } else if (strcmp(mode, "remote-ahead") == 0) {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    require(result.pushed == 0, "does not upload a position behind the server");
    require(result.parked == 1 && !SyncParked::empty(), "parks the further-ahead position");
    require(!progressPending(FIRST), "clears the bit of a position that can never be uploaded");
  } else if (strcmp(mode, "server-error") == 0) {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    require(result.pushed == 0 && result.failed == 1, "reports the failed upload");
    require(progressPending(FIRST), "keeps the pending bit after a 500");
  } else if (batch) {
    require(result.status == AutoProgressSync::Result::Status::Ran, "ran");
    // Either one batch request, or one PUT per book when the server has no
    // batch route; both confirm every book.
    require(result.pushed == 2, "uploads both books");
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
