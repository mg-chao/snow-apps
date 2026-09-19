#include "snow_shot/storage/storageusagetracker.h"

#include "snow_shot/storage/storagelogging.h"
#include "pinnedwindowstorageconstants_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSet>

#include <algorithm>
#include <utility>

#include "storagedirectoryutils_p.h"

namespace snow_shot::storage {
namespace {
// Keep in sync with the directory names used by the owning repositories and
// services: capturehistoryrepository.cpp, pinnedwindowrepository.cpp, and the
// OCR asset cache root (configurationDirectory()/assets).
constexpr auto kHistoryDirectoryName = "capture_history";
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
      m_diagnosticsDirectories(std::move(options.diagnosticsDirectories)),
      m_activeFileCutoff(options.activeFileCutoff),
      m_historyBytesProvider(std::move(options.historyBytesProvider)),
      m_directoryScanObserved(std::move(options.directoryScanObserved)),
      m_callbacks(std::move(options.callbacks)) {}

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

void StorageUsageTracker::requestRefreshIfStale(std::chrono::milliseconds maxAge) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_lastScanFinished != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - m_lastScanFinished < maxAge) {
            return;
        }
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
    // The capture-history repository maintains its byte total incrementally, so
    // walking its directories again would duplicate a scan on every refresh.
    const bool historyProvided = m_historyBytesProvider != nullptr;
    qint64 providedHistoryBytes = 0;
    if (historyProvided) {
        providedHistoryBytes = std::max<qint64>(0, m_historyBytesProvider());
    }
    const QFileInfoList entries =
        QDir(m_appDataDirectory)
            .entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                           QDir::System);
    for (const QFileInfo& entry : entries) {
        if (storageLink(entry)) {
            continue;
        }
        const QString name = entry.fileName();
        const bool history = name == QLatin1String(kHistoryDirectoryName);
        if (history && historyProvided) {
            continue;
        }
        if (entry.isDir() && m_directoryScanObserved) {
            m_directoryScanObserved(entry.absoluteFilePath());
        }
        const qint64 bytes =
            entry.isDir() ? directoryBytes(entry.absoluteFilePath()) : entry.size();
        if (history) {
            if (!historyProvided) {
                scanned.historyBytes += bytes;
            }
        } else if (name == QLatin1String(pinned_window_storage::kDirectoryName)) {
            scanned.pinnedWindowBytes += bytes;
        } else if (name == QLatin1String(kAssetDirectoryName)) {
            scanned.ocrAssetBytes += bytes;
        } else if (name == QLatin1String("logs")) {
            scanned.diagnosticsBytes += bytes;
        } else {
            scanned.otherBytes += bytes;
        }
    }
    if (historyProvided) {
        scanned.historyBytes = providedHistoryBytes;
    }
    scanned.thumbnailCacheBytes = directoryBytes(m_thumbnailCacheDirectory);
    const QString insideRoot = QDir(m_appDataDirectory).filePath(QStringLiteral("logs"));
    QStringList counted{QDir::cleanPath(insideRoot).toCaseFolded()};
    QStringList roots = m_diagnosticsDirectories;
    std::sort(roots.begin(), roots.end(),
              [](const QString& a, const QString& b) { return a.size() < b.size(); });
    for (const QString& root : roots) {
        const QString normalized = QDir::cleanPath(root).toCaseFolded();
        const bool covered =
            std::any_of(counted.begin(), counted.end(), [&](const QString& parent) {
                return normalized == parent || normalized.startsWith(parent + u'/');
            });
        if (covered)
            continue;
        qint64 bytes = directoryBytes(root);
        const QString appRoot = QDir::cleanPath(m_appDataDirectory).toCaseFolded();
        if (appRoot == normalized || appRoot.startsWith(normalized + u'/'))
            bytes -= directoryBytes(m_appDataDirectory);
        else if (counted.front().startsWith(normalized + u'/'))
            bytes -= directoryBytes(insideRoot);
        scanned.diagnosticsBytes += std::max<qint64>(0, bytes);
        counted.append(normalized);
    }
    scanned.recordingTempBytes = directoryBytes(m_recordingTempDirectory);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_usage = scanned;
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_lastScanFinished = std::chrono::steady_clock::now();
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
