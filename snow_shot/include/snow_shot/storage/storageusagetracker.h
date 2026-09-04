#ifndef SNOW_SHOT_STORAGE_STORAGEUSAGETRACKER_H
#define SNOW_SHOT_STORAGE_STORAGEUSAGETRACKER_H

#include "snow_shot/storage/appstorageusage.h"
#include "snow_shot/storage/storageresult.h"

#include <QDateTime>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace snow_shot::storage {

struct StorageUsageTrackerOptions {
    QString appDataDirectory;
    QString thumbnailCacheDirectory;
    QString recordingTempDirectory;
    // Entries in the recording-temp directory modified at or after this
    // timestamp belong to the currently running session and are never deleted.
    QDateTime activeFileCutoff;
    struct Callbacks {
        std::function<void(const AppStorageUsage&)> usageChanged;
        std::function<void(StorageCacheKind kind, const StorageResult& result)> clearFinished;
    } callbacks;
};

// Scans the app-owned storage locations on a worker thread and keeps a cached
// AppStorageUsage snapshot available for synchronous readers.  Construction
// neither scans nor spawns a thread; the first scan runs when a refresh is
// requested, and the worker thread exits once the pending work drains.
// Refresh requests are coalesced; cache-clear commands run in submission order
// and trigger a rescan before they report completion.
class StorageUsageTracker final {
  public:
    explicit StorageUsageTracker(StorageUsageTrackerOptions options);
    ~StorageUsageTracker();

    StorageUsageTracker(const StorageUsageTracker&) = delete;
    StorageUsageTracker& operator=(const StorageUsageTracker&) = delete;

    [[nodiscard]] AppStorageUsage usage() const;
    void requestRefresh();
    [[nodiscard]] std::shared_future<StorageResult> requestClear(StorageCacheKind kind);
    void drain();

    [[nodiscard]] static QString defaultThumbnailCacheDirectory();
    [[nodiscard]] static QString defaultRecordingTempDirectory();

  private:
    struct Command {
        StorageCacheKind kind = StorageCacheKind::ThumbnailCache;
        std::shared_ptr<std::promise<StorageResult>> resultPromise;
    };

    void workerLoop();
    // Requires m_queueMutex.  Spawns the worker unless one is already alive.
    // A false m_workerLive means the previous worker returned from workerLoop,
    // so the join below never blocks.
    void ensureWorkerLocked();
    [[nodiscard]] AppStorageUsage scanNow();
    [[nodiscard]] StorageResult clearNow(StorageCacheKind kind);

    QString m_appDataDirectory;
    QString m_thumbnailCacheDirectory;
    QString m_recordingTempDirectory;
    QDateTime m_activeFileCutoff;
    StorageUsageTrackerOptions::Callbacks m_callbacks;

    mutable std::mutex m_stateMutex;
    AppStorageUsage m_usage;

    std::deque<Command> m_queue;
    bool m_refreshPending = false;
    bool m_stopping = false;
    bool m_workerLive = false;
    std::mutex m_queueMutex;
    std::condition_variable m_drainCondition;
    std::thread m_worker;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_STORAGEUSAGETRACKER_H
