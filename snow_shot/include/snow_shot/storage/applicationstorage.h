#ifndef SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H
#define SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H

#include "snow_shot/storage/appstorageusage.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/storageresult.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <future>

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
};

enum class StorageMode {
    ApplicationData,
    Portable,
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

class ApplicationStorage final : public QObject {
    Q_OBJECT

  public:
    static ApplicationStorage& instance();
    static StorageDirectorySelection
    resolveDirectory(const StorageInitializationOptions& options = {});
    ~ApplicationStorage() override;

    [[nodiscard]] StorageResult initialize(const StorageInitializationOptions& options = {});
    [[nodiscard]] bool isInitialized() const;
    [[nodiscard]] StorageResult flushNow();
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
    [[nodiscard]] bool smartSelectionEnabled() const;

    bool requestCaptureHistoryPolicy(const CaptureHistoryPolicy& policy);
    [[nodiscard]] std::shared_future<StorageResult>
    requestCaptureHistoryPolicyAsync(const CaptureHistoryPolicy& policy);
    bool requestSmartSelection(bool enabled);
    [[nodiscard]] std::shared_future<StorageResult> requestSmartSelectionAsync(bool enabled);
    [[nodiscard]] PinnedWindowPolicy pinnedWindowPolicy() const;
    bool requestPinnedWindowPolicy(const PinnedWindowPolicy& policy);
    bool requestPinnedWindowClear();
    void requestPinnedWindowShow(const QString& id) {
        emit pinnedWindowShowRequested(id);
    }
    void requestPinnedWindowDelete(const QVector<QString>& ids) {
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
    void captureHistoryChanged();
    void pinnedWindowsChanged();
    void pinnedWindowShowRequested(const QString& id);
    void pinnedWindowDeleteRequested(const QVector<QString>& ids);
    void storageStatusChanged(const snow_shot::storage::StorageStatus& status);
    void smartSelectionChanged(bool enabled);
    void captureHistoryClearFinished(bool success, const QString& error);

  private:
    explicit ApplicationStorage(QObject* parent = nullptr);
    void updateConfigurationError(const QString& error);
    void updateHistoryError(const QString& error);
    void updateHistoryUsage(const CaptureHistoryUsage& usage);
    void updateAppUsage(const AppStorageUsage& usage);
    void finishHistoryClear(bool success, const QString& error);
    void finishHistoryPolicy(bool success, const QString& error);
    void finishCacheClear(StorageCacheKind kind, const StorageResult& result);
    void emitStatusChanged();

    StorageStatus m_status;
    std::unique_ptr<ConfigurationStore> m_configuration;
    std::unique_ptr<CaptureHistoryRepository> m_captureHistory;
    std::unique_ptr<PinnedWindowRepository> m_pinnedWindows;
    QThreadPool m_pinnedPreviewPool;
    QThreadPool m_pinnedFullImagePool;
    std::unique_ptr<StorageUsageTracker> m_usageTracker;
    std::atomic_bool m_pinnedChangeQueued{false};
    quint64 m_lastPinnedNotifiedRevision = 0;
    bool m_initialized = false;
};
} // namespace snow_shot::storage

Q_DECLARE_METATYPE(snow_shot::storage::CaptureHistoryUsage)
Q_DECLARE_METATYPE(snow_shot::storage::StorageStatus)

#endif // SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H
