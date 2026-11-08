#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/platform/applicationqos.h"
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

QString cleanDirectory(const QString& path) {
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

QString defaultCacheDirectory() {
    if (app::distribution::isStoreManaged()) {
        const QString localState = app::distribution::writableDataDirectory();
        return localState.isEmpty() ? QString()
                                    : QDir(localState).filePath(QStringLiteral("cache"));
    }
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString failureMessage(StorageCacheKind kind) {
    return kind == StorageCacheKind::ThumbnailCache
               ? QStringLiteral("Some thumbnail-cache files could not be removed")
               : QStringLiteral("Some recording temporary files could not be removed");
}
} // namespace

StorageUsageTracker::StorageUsageTracker(StorageUsageTrackerOptions options)
    : m_appDataDirectory(cleanDirectory(options.appDataDirectory)),
      m_thumbnailCacheDirectory(cleanDirectory(options.thumbnailCacheDirectory)),
      m_recordingTempDirectory(cleanDirectory(options.recordingTempDirectory)),
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
    const QString root = defaultCacheDirectory();
    return root.isEmpty() ? QString()
                          : QDir(root).filePath(QStringLiteral("snow-shot/history-thumbnails"));
}

QString StorageUsageTracker::defaultRecordingTempDirectory() {
    QString root = defaultCacheDirectory();
    if (root.isEmpty() && !app::distribution::isStoreManaged()) {
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
    m_worker = std::thread(snow_shot::runtime::trackRuntimeWork([this]() {
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        workerLoop();
    }));
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
    scanned.thumbnailCacheBytes = directoryBytes(m_thumbnailCacheDirectory);
    scanned.recordingTempBytes = directoryBytes(m_recordingTempDirectory);
    const QString insideRoot = m_appDataDirectory.isEmpty()
                                   ? QString()
                                   : QDir(m_appDataDirectory).filePath(QStringLiteral("logs"));
    bool insideLogsCounted = false;
    const QFileInfoList entries =
        m_appDataDirectory.isEmpty()
            ? QFileInfoList()
            : QDir(m_appDataDirectory)
                  .entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                                 QDir::System);
    for (const QFileInfo& entry : entries) {
        if (storageLink(entry)) {
            continue;
        }
        const QString name = entry.fileName();
        if (name == QLatin1String("logs")) {
            const QString normalized = QDir::cleanPath(entry.absoluteFilePath()).toCaseFolded();
            const bool configured =
                std::any_of(m_diagnosticsDirectories.cbegin(), m_diagnosticsDirectories.cend(),
                            [&](const QString& root) {
                                if (root.isEmpty())
                                    return false;
                                const QString parent = QDir::cleanPath(root).toCaseFolded();
                                return normalized == parent || normalized.startsWith(parent + u'/');
                            });
            // A former custom/portable data directory can still contain old logs.
            // Only the explicitly configured diagnostics roots belong to this session.
            if (!configured)
                continue;
            insideLogsCounted = true;
        }
        const bool history = name == QLatin1String(kHistoryDirectoryName);
        if (history && historyProvided) {
            continue;
        }
        if (entry.isDir() && m_directoryScanObserved) {
            m_directoryScanObserved(entry.absoluteFilePath());
        }
        qint64 bytes = entry.isDir() ? directoryBytes(entry.absoluteFilePath()) : entry.size();
        if (entry.isDir()) {
            const QString root = QDir::cleanPath(entry.absoluteFilePath());
            // Store caches live inside LocalState, while their usage is reported
            // separately. Do not count those same files again as application data.
            if (!m_thumbnailCacheDirectory.isEmpty() &&
                (m_thumbnailCacheDirectory == root ||
                 m_thumbnailCacheDirectory.startsWith(root + u'/'))) {
                bytes -= scanned.thumbnailCacheBytes;
            }
            if (!m_recordingTempDirectory.isEmpty() &&
                (m_recordingTempDirectory == root ||
                 m_recordingTempDirectory.startsWith(root + u'/'))) {
                bytes -= scanned.recordingTempBytes;
            }
            bytes = std::max<qint64>(0, bytes);
        }
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
    QStringList counted;
    if (insideLogsCounted)
        counted.append(QDir::cleanPath(insideRoot).toCaseFolded());
    QStringList roots = m_diagnosticsDirectories;
    std::sort(roots.begin(), roots.end(),
              [](const QString& a, const QString& b) { return a.size() < b.size(); });
    for (const QString& root : roots) {
        if (root.isEmpty())
            continue;
        const QString normalized = QDir::cleanPath(root).toCaseFolded();
        const bool covered =
            std::any_of(counted.begin(), counted.end(), [&](const QString& parent) {
                return normalized == parent || normalized.startsWith(parent + u'/');
            });
        if (covered)
            continue;
        qint64 bytes = directoryBytes(root);
        const QString appRoot = QDir::cleanPath(m_appDataDirectory).toCaseFolded();
        if (!m_appDataDirectory.isEmpty() &&
            (appRoot == normalized || appRoot.startsWith(normalized + u'/')))
            bytes -= directoryBytes(m_appDataDirectory);
        else if (insideLogsCounted &&
                 QDir::cleanPath(insideRoot).toCaseFolded().startsWith(normalized + u'/'))
            bytes -= directoryBytes(insideRoot);
        scanned.diagnosticsBytes += std::max<qint64>(0, bytes);
        counted.append(normalized);
    }

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
    const QString& path = kind == StorageCacheKind::ThumbnailCache ? m_thumbnailCacheDirectory
                                                                   : m_recordingTempDirectory;
    if (path.isEmpty())
        return StorageResult::ok();
    if (kind == StorageCacheKind::ThumbnailCache) {
        QDir directory(m_thumbnailCacheDirectory);
        const QFileInfo root(m_thumbnailCacheDirectory);
        if (!root.exists() || storageLink(root)) {
            return StorageResult::ok();
        }
        if (!root.isDir() || !directory.isReadable()) {
            return StorageResult::failure(failureMessage(kind));
        }
        const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot |
                                                              QDir::Hidden | QDir::System);
        int failures = 0;
        for (const QFileInfo& entry : entries) {
            if (storageLink(entry) ||
                (!entry.fileName().endsWith(QLatin1String(".png"), Qt::CaseInsensitive) &&
                 !entry.fileName().endsWith(QLatin1String(".png.size"), Qt::CaseInsensitive))) {
                continue;
            }
            if (!QFile::remove(entry.absoluteFilePath())) {
                ++failures;
            }
        }
        if (directory.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                              QDir::System) &&
            !QDir().rmdir(m_thumbnailCacheDirectory)) {
            ++failures;
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
