#ifndef SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H
#define SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H

#include "snow_shot/storage/appstorageusage.h"
#include "snow_shot/storage/storagedirectorychange.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/storageresult.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include "snow_shot/smartselectionmode.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <future>
#include <mutex>

namespace snow_shot::storage {
class CaptureHistoryRepository;
class PinnedWindowRepository;
class StorageUsageTracker;
struct StorageDirectorySelection;

struct StorageInitializationOptions {
    QString executableDirectory;
    QString appDataDirectory;
    int debounceMilliseconds = 1000;
    std::shared_ptr<const StorageDirectorySelection> resolvedDirectory{};
    // Health probes use only their temporary repository root. Do not scan or clean
    // application-global thumbnail, recording-temp, or diagnostics directories.
    bool isolateAuxiliaryStorage = false;
};

enum class StorageMode {
    ApplicationData,
    Portable,
    Custom,
    FutureVersionReadOnly,
    Degraded,
};

struct StorageDirectorySelection {
    QString requestedDirectory;
    QString effectiveDirectory;
    QString fallbackReason;
    QString executableDirectory;
    StorageMode mode = StorageMode::Degraded;
};

struct StorageStatus {
    QString requestedDirectory;
    QString effectiveDirectory;
    QString fallbackReason;
    StorageMode effectiveMode = StorageMode::Degraded;
    ConfigurationCompatibility configurationCompatibility = ConfigurationCompatibility::Unavailable;
    bool directoryChanging = false;
    bool readAvailable = false;
    bool writeAvailable = false;
    CaptureHistoryUsage historyUsage;
    AppStorageUsage appUsage;
    bool historyPolicyUpdating = false;
    bool pinnedPolicyUpdating = false;
    bool pinnedClearing = false;
    QString lastPinnedError;
    bool historyClearing = false;
    bool cacheClearing = false;
    QString lastConfigurationError;
    QString lastHistoryError;
    diagnostics::DiagnosticsStatus diagnostics;
};

struct StorageCheckpointResult {
    StorageResult configuration;
    StorageResult pinnedWindows;
    // Draining history waits for admitted operations. Its last operation error is
    // informational, not a failure to persist work newly requested by this checkpoint.
    QString historyError;

    [[nodiscard]] StorageResult result() const {
        return pinnedWindows.success ? configuration : pinnedWindows;
    }
};

class ApplicationStorage final : public QObject {
    Q_OBJECT

  public:
    using DirectoryChangeTask = std::function<StorageDirectoryChangeResult()>;
    // The executor starts work asynchronously at most once and returns a future that becomes
    // ready only after work has stopped using storage. Throwing or returning an invalid future
    // must leave no work in flight. Empty selects the production std::async executor.
    using DirectoryChangeExecutor =
        std::function<std::future<StorageDirectoryChangeResult>(DirectoryChangeTask)>;
    static ApplicationStorage& instance();
    static StorageDirectorySelection
    resolveDirectory(const StorageInitializationOptions& options = {});
    ~ApplicationStorage() override;

    [[nodiscard]] StorageResult initialize(const StorageInitializationOptions& options = {});
    [[nodiscard]] bool isInitialized() const;
    // Owner-thread checkpoint: snapshot GUI metadata, drain existing history/cache work,
    // and attempt both independent writes. No cross-repository atomicity is promised.
    [[nodiscard]] StorageCheckpointResult checkpoint();
    // Compatibility aggregate: pin failure takes precedence, then configuration failure.
    [[nodiscard]] StorageResult flushNow();
    // Suspend/resume run on the owner thread; drain runs on the worker. A failed suspend
    // leaves participants unchanged. Suspend and resume must not throw. Resume receives
    // the authoritative root and runs once after an acquired suspension, including failure.
    void setDirectoryChangeHooks(std::function<StorageResult()> suspend,
                                 std::function<void(const QString&)> resume,
                                 std::function<void()> drain = {},
                                 DirectoryChangeExecutor executor = {});
    [[nodiscard]] StorageResult requestDirectoryChange(const QString& directory, bool migrate);
    // Owner-thread teardown barrier: finish the worker and restore participants before
    // replacing hooks or destroying the objects borrowed by them.
    void waitForDirectoryChange();
    [[nodiscard]] bool directoryChanging() const {
        return m_status.directoryChanging;
    }
    void shutdown();

    [[nodiscard]] ConfigurationStore& configuration();
    [[nodiscard]] CaptureHistoryRepository& captureHistory();
    [[nodiscard]] PinnedWindowRepository& pinnedWindows();
    [[nodiscard]] QThreadPool& pinnedPreviewPool() {
        return m_pinnedPreviewPool;
    }
    [[nodiscard]] QThreadPool& pinnedFullImagePool() {
        return m_pinnedFullImagePool;
    }
    [[nodiscard]] StorageStatus status() const;
    [[nodiscard]] CaptureHistoryPolicy captureHistoryPolicy() const;
    [[nodiscard]] QString configurationDirectory() const;
    [[nodiscard]] SmartSelectionMode smartSelectionMode() const;

    bool requestCaptureHistoryPolicy(const CaptureHistoryPolicy& policy);
    [[nodiscard]] std::shared_future<StorageResult>
    requestCaptureHistoryPolicyAsync(const CaptureHistoryPolicy& policy);
    bool requestSmartSelectionMode(SmartSelectionMode mode);
    [[nodiscard]] std::shared_future<StorageResult>
    requestSmartSelectionModeAsync(SmartSelectionMode mode);
    [[nodiscard]] PinnedWindowPolicy pinnedWindowPolicy() const;
    bool requestPinnedWindowPolicy(const PinnedWindowPolicy& policy);
    bool requestPinnedWindowClear();
    void requestPinnedWindowRetentionCleanup(bool restartsQuiet = true);
    void requestPinnedWindowShow(const QString& id) {
        if (!directoryChangeBlocksWork())
            emit pinnedWindowShowRequested(id);
    }
    void requestPinnedWindowsShow(const QStringList& ids) {
        if (!directoryChangeBlocksWork())
            emit pinnedWindowsShowRequested(ids);
    }
    void requestPinnedWindowDelete(const QVector<QString>& ids) {
        if (!directoryChangeBlocksWork())
            emit pinnedWindowDeleteRequested(ids);
    }
    bool requestCaptureHistoryClear();
    [[nodiscard]] std::shared_future<StorageResult> requestCaptureHistoryClearAsync();

    void requestStorageUsageRefresh();
    // Rescans only when the cached usage snapshot is older than the freshness
    // window; the settings page uses this on show events, while the explicit
    // refresh button always rescans through requestStorageUsageRefresh().
    void requestStorageUsageRefreshIfStale();
    bool requestThumbnailCacheClear();
    [[nodiscard]] std::shared_future<StorageResult> requestThumbnailCacheClearAsync();
    bool requestRecordingTempClear();
    [[nodiscard]] std::shared_future<StorageResult> requestRecordingTempClearAsync();

  signals:
    // Synchronous GUI-thread checkpoint before repositories are drained. Worker flushes
    // consume already captured metadata and never call window-system APIs.
    void checkpointRequested();
    // Latest UI snapshot; intermediate progress may be coalesced while the owner is busy.
    // The last pending snapshot is delivered before directoryChangeFinished, never after it.
    void directoryChangeProgress(const snow_shot::storage::StorageDirectoryProgress& progress);
    void directoryChangeFinished(const snow_shot::storage::StorageDirectoryChangeResult& result);
    void captureHistoryChanged();
    void pinnedWindowsChanged();
    void pinnedWindowShowRequested(const QString& id);
    void pinnedWindowsShowRequested(const QStringList& ids);
    void pinnedWindowDeleteRequested(const QVector<QString>& ids);
    void storageStatusChanged(const snow_shot::storage::StorageStatus& status);
    void smartSelectionModeChanged(snow_shot::SmartSelectionMode mode);
    void captureHistoryClearFinished(bool success, const QString& error);
    void cacheClearFinished(snow_shot::storage::StorageCacheKind kind, bool success);

  private:
    explicit ApplicationStorage(QObject* parent = nullptr);
    static void releaseApplicationResources();
    void drainRuntimeWork();
    void updateConfigurationError(const QString& error);
    void updateHistoryError(const QString& error);
    void updateHistoryUsage(const CaptureHistoryUsage& usage);
    void updateAppUsage(const AppStorageUsage& usage);
    void finishHistoryClear(bool success, const QString& error);
    void finishHistoryPolicy(bool success, const QString& error);
    void finishCacheClear(StorageCacheKind kind, const StorageResult& result);
    void emitStatusChanged();
    void createUsageTracker();
    void refreshStorageUsage();
    void finishDirectoryChange();
    [[nodiscard]] bool directoryChangeBlocksWork() const;
    struct DirectoryChange;
    quint64 m_storageGeneration = 0;
    quint64 m_usageGeneration = 0;
    QString m_bootstrapDirectory;
    QString m_executableDirectory;
    bool m_isolateAuxiliaryStorage = false;
    std::function<StorageResult()> m_suspendForDirectoryChange;
    std::function<void()> m_drainForDirectoryChange;
    std::function<void(const QString&)> m_resumeAfterDirectoryChange;
    DirectoryChangeExecutor m_directoryChangeExecutor;
    std::unique_ptr<DirectoryChange> m_directoryChange;
    bool m_checkpointInProgress = false;
    bool m_preparingDirectoryChange = false;
    bool m_finishingDirectoryChange = false;

    StorageStatus m_status;
    std::unique_ptr<ConfigurationStore> m_configuration;
    std::unique_ptr<QTimer> m_pinnedCleanupTimer;
    std::unique_ptr<CaptureHistoryRepository> m_captureHistory;
    std::unique_ptr<PinnedWindowRepository> m_pinnedWindows;
    QThreadPool m_pinnedPreviewPool;
    QThreadPool m_pinnedFullImagePool;
    QThreadPool m_pinnedMaintenancePool;
    std::unique_ptr<StorageUsageTracker> m_usageTracker;
    std::atomic_bool m_pinnedChangeQueued{false};
    std::mutex m_pinnedMaintenanceMutex;
    bool m_pinnedMaintenancePending = false;
    bool m_pinnedMaintenanceRunning = false;
    quint64 m_lastPinnedNotifiedRevision = 0;
    QMetaObject::Connection m_aboutToQuitConnection;
    bool m_initialized = false;
};
} // namespace snow_shot::storage

Q_DECLARE_METATYPE(snow_shot::storage::CaptureHistoryUsage)
Q_DECLARE_METATYPE(snow_shot::storage::StorageStatus)

#endif // SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H
