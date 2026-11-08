#pragma once

#include <KOReaderSyncClient.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "StatsUploadClient.h"

struct BookReadingStats;

// Foreground helpers shared by reader sync and the explicit folder upload.
namespace ReadingSyncUpload {
// Whether a saved progress record can be mapped. `tolerateLegacy` mirrors the
// manual Sync Book path (KOReaderSyncActivity::ensureLocalProgressLoaded): a
// record written by older firmware is the 6/10-byte form with no page count,
// and an EPUB replaced under the same path can leave a stale spine index.
// Without it the old, stricter rule applies. Pure so the rule is tested alone.
inline bool usableSavedProgress(const bool hasPageCount, const int pageCount, const int pageNumber,
                                const int spineIndex, const int spineCount, const bool tolerateLegacy) {
  if (spineCount <= 0 || pageCount < 1) return false;
  if (!hasPageCount && !tolerateLegacy) return false;
  if (hasPageCount && pageNumber >= pageCount) return false;
  if (!tolerateLegacy && (spineIndex < 0 || spineIndex >= spineCount)) return false;
  return true;
}

// Only saved, mappable EPUB positions qualify. Never invent progress for an unread book.
// `tolerateLegacy` is used by the sleep sync so a book the manual sync accepts is
// not permanently refused by the automatic one.
bool prepareProgress(const std::string& path, KOReaderProgress& progress, bool tolerateLegacy = false);
struct ExtrasResult {
  StatsUploadClient::Result stats = StatsUploadClient::Result::Skipped;
  StatsUploadClient::Result clippings = StatsUploadClient::Result::Skipped;
  bool success() const { return !StatsUploadClient::failed(stats) && !StatsUploadClient::failed(clippings); }
};
enum class GlobalStatsStep : uint8_t { None, Setup, Totals, Daily, Sessions, Streak };
// Reports the step that could not finish; success/skipped clears any previous failure.
StatsUploadClient::Result globalStats(GlobalStatsStep* failedStep = nullptr);
// Reuse the explicit stats activity's existing buffer/client rather than allocating a second pair.
StatsUploadClient::Result globalStats(StatsUploadClient& client, char* payload, size_t capacity, const char* deviceId,
                                      GlobalStatsStep* failedStep = nullptr);
StatsUploadClient::Result stats(const std::string& path, const std::string& document);
// A book's saved stats when there are any to send (stats tracking on for it).
// Only a stats-file existence check and one small read; never loads the book.
bool loadBookStats(const std::string& path, BookReadingStats& stats);
// Targeted upload actions appear only with credentials, a server confirmed to
// accept stats and clippings (an unknown custom server waits for Authenticate
// or a first upload), and the matching What to Sync toggle on.
bool canSyncReadingStats();
bool canSyncClippings();
ExtrasResult extras(const std::string& path, const std::string& document, bool (*cancel)(void*) = nullptr,
                    void* context = nullptr);
// Shared result wording for every sync screen: "Not supported" on a progress-only
// server, "OFF" when the user turned that data off, otherwise the upload result.
const char* statusLabel(StatsUploadClient::Result result, bool clippings);
// All-time result includes the failed step, using caller-owned bounded storage.
const char* globalStatusLabel(char* buffer, size_t capacity, StatsUploadClient::Result result, GlobalStatsStep step);
// Bulk variant: "3" or "3 (1 failed)", with the same OFF / Not supported states.
const char* countLabel(char* buffer, size_t capacity, uint32_t ok, uint32_t failed, bool clippings);
// Heading for a book that failed to sync: names the part that failed
// ("Progress sync failed"), or "Sync failed" when several (or none known) did.
const char* failureHeading(bool progress, bool stats, bool clippings);
// The book's file name without folder or extension, for failure screens that
// have no EPUB metadata loaded.
std::string displayName(const std::string& bookPath);
}  // namespace ReadingSyncUpload
