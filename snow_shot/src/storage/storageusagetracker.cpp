#include "snow_shot/storage/storageusagetracker.h"

#include "snow_shot/storage/storagelogging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <utility>

#include "storagedirectoryutils_p.h"

namespace snow_shot::storage {
namespace {
// Keep in sync with the directory names used by the owning repositories and
// services: capturehistoryrepository.cpp, pinnedwindowrepository.cpp, and the
// OCR asset cache root (configurationDirectory()/assets).
constexpr auto kHistoryDirectoryName = "capture_history_records";
constexpr auto kQuarantineDirectoryName = "capture_history_quarantine";
constexpr auto kPinnedWindowDirectoryName = "pinned_windows_v3";
constexpr auto kAssetDirectoryName = "assets";
constexpr auto kThumbnailSuffix = "png";

QString failureMessage(StorageCacheKind kind) {
    return kind == StorageCacheKind::ThumbnailCache
               ? QStringLiteral("Some thumbnail-cache files could not be removed")
               : QStringLiteral("Some recording temporary files could not be removed");
}
} // namespace

StorageUsageTracker::StorageUsageTracker(StorageUsageTrackerOptions options)
    : m_appDataDirectory(QDir::cleanPath(options.appDataDirectory)),
      m_thumbnailCacheDirectory(QDir::cleanPath(options.thumbnailCacheDirectory)),
      m_recordingTempDirectory(QDir::cleanPath(options.recordingTempDirectory)),
      m_activeFileCutoff(options.activeFileCutoff), m_callbacks(std::move(options.callbacks)) {}

StorageUsageTracker::~StorageUsageTracker() {
    drain();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopping = true;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

AppStorageUsage StorageUsageTracker::usage() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_usage;
}

void StorageUsageTracker::requestRefresh() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_refreshPending = true;
        ensureWorkerLocked();
    }
}

std::shared_future<StorageResult> StorageUsageTracker::requestClear(StorageCacheKind kind) {
    auto promise = std::make_shared<std::promise<StorageResult>>();
    std::shared_future<StorageResult> future = promise->get_future().share();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        Command command;
        command.kind = kind;
        command.resultPromise = std::move(promise);
        m_queue.push_back(std::move(command));
        ensureWorkerLocked();
    }
    return future;
}

void StorageUsageTracker::drain() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_drainCondition.wait(
        lock, [this]() { return !m_workerLive && m_queue.empty() && !m_refreshPending; });
}

QString StorageUsageTracker::defaultThumbnailCacheDirectory() {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return root.isEmpty() ? QString()
                          : QDir(root).filePath(QStringLiteral("snow-shot/history-thumbnails"));
}

QString StorageUsageTracker::defaultRecordingTempDirectory() {
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("recordings"));
}

void StorageUsageTracker::ensureWorkerLocked() {
    if (m_workerLive || m_stopping) {
        return;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::thread([this]() { workerLoop(); });
    m_workerLive = true;
}

void StorageUsageTracker::workerLoop() {
    // The worker is only ever spawned with work already queued, so the loop
    // never waits: it processes whatever arrived and exits once the queue is
    // empty again.
    for (;;) {
        Command command;
        bool hasCommand = false;
        bool refresh = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_queue.empty()) {
                command = std::move(m_queue.front());
                m_queue.pop_front();
                hasCommand = true;
            } else if (m_refreshPending) {
                m_refreshPending = false;
                refresh = true;
            } else {
                m_workerLive = false;
                m_drainCondition.notify_all();
                return;
            }
        }

        if (hasCommand) {
            const StorageResult result = clearNow(command.kind);
            if (command.resultPromise != nullptr) {
                command.resultPromise->set_value(result);
            }
            // The clear changed disk usage, so rescan before reporting idle.
            static_cast<void>(scanNow());
            if (m_callbacks.clearFinished) {
                m_callbacks.clearFinished(command.kind, result);
            }
        } else if (refresh) {
            static_cast<void>(scanNow());
        }
    }
}

AppStorageUsage StorageUsageTracker::scanNow() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_usage.scanning = true;
    }
    if (m_callbacks.usageChanged) {
        m_callbacks.usageChanged(usage());
    }

    AppStorageUsage scanned;
    const QFileInfoList entries =
        QDir(m_appDataDirectory)
            .entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                           QDir::System);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        const qint64 bytes =
            entry.isDir() ? directoryBytes(entry.absoluteFilePath()) : entry.size();
        const QString name = entry.fileName();
        if (name == QLatin1String(kHistoryDirectoryName) ||
            name == QLatin1String(kQuarantineDirectoryName)) {
            scanned.historyBytes += bytes;
        } else if (name == QLatin1String(kPinnedWindowDirectoryName)) {
            scanned.pinnedWindowBytes += bytes;
        } else if (name == QLatin1String(kAssetDirectoryName)) {
            scanned.ocrAssetBytes += bytes;
        } else {
            scanned.otherBytes += bytes;
        }
    }
    scanned.thumbnailCacheBytes = directoryBytes(m_thumbnailCacheDirectory);
    scanned.recordingTempBytes = directoryBytes(m_recordingTempDirectory);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_usage = scanned;
    }
    if (m_callbacks.usageChanged) {
        m_callbacks.usageChanged(scanned);
    }
    return scanned;
}

StorageResult StorageUsageTracker::clearNow(StorageCacheKind kind) {
    if (kind == StorageCacheKind::ThumbnailCache) {
        QDir directory(m_thumbnailCacheDirectory);
        if (!directory.exists()) {
            return StorageResult::ok();
        }
        const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot |
                                                              QDir::Hidden | QDir::System);
        int failures = 0;
        for (const QFileInfo& entry : entries) {
            if (entry.isSymLink() ||
                entry.suffix().compare(QLatin1String(kThumbnailSuffix), Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (!QFile::remove(entry.absoluteFilePath())) {
                ++failures;
            }
        }
        return failures == 0 ? StorageResult::ok() : StorageResult::failure(failureMessage(kind));
    }

    QDir directory(m_recordingTempDirectory);
    if (!directory.exists()) {
        return StorageResult::ok();
    }
    const QFileInfoList entries = directory.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    int failures = 0;
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        if (m_activeFileCutoff.isValid() && entry.lastModified() >= m_activeFileCutoff) {
            continue;
        }
        const bool removed = entry.isDir() ? QDir(entry.absoluteFilePath()).removeRecursively()
                                           : QFile::remove(entry.absoluteFilePath());
        if (!removed) {
            ++failures;
        }
    }
    return failures == 0 ? StorageResult::ok() : StorageResult::failure(failureMessage(kind));
}
} // namespace snow_shot::storage
