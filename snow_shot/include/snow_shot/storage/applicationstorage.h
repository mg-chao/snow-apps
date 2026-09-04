#ifndef SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H
#define SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H

#include "snow_shot/storage/appstorageusage.h"
#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/storageresult.h"

#include <QObject>
#include <QString>

#include <memory>
#include <future>

namespace snow_shot::storage {
class CaptureHistoryRepository;
class PinnedWindowRepository;
class StorageUsageTracker;

struct StorageInitializationOptions {
    QString executableDirectory;
    QString appDataDirectory;
    int debounceMilliseconds = 1000;
};

enum class StorageMode {
    ApplicationData,
    Portable,
    FutureVersionReadOnly,
    Degraded,
};

struct StorageStatus {
    QString requestedDirectory;
    QString effectiveDirectory;
    QString fallbackReason;
    StorageMode effectiveMode = StorageMode::Degraded;
    ConfigurationCompatibility configurationCompatibility =
        ConfigurationCompatibility::Unavailable;
    bool readAvailable = false;
    bool writeAvailable = false;
    CaptureHistoryUsage historyUsage;
    AppStorageUsage appUsage;
    bool historyPolicyUpdating = false;
    bool historyClearing = false;
    bool cacheClearing = false;
    QString lastConfigurationError;
    QString lastHistoryError;
};

class ApplicationStorage final : public QObject {
    Q_OBJECT

  public:
    static ApplicationStorage& instance();
    ~ApplicationStorage() override;

    [[nodiscard]] StorageResult initialize(const StorageInitializationOptions& options = {});
    [[nodiscard]] bool isInitialized() const;
    [[nodiscard]] StorageResult flushNow();
    void shutdown();

    [[nodiscard]] ConfigurationStore& configuration();
    [[nodiscard]] CaptureHistoryRepository& captureHistory();
    [[nodiscard]] PinnedWindowRepository& pinnedWindows();
    [[nodiscard]] StorageStatus status() const;
    [[nodiscard]] CaptureHistoryPolicy captureHistoryPolicy() const;
    [[nodiscard]] QString configurationDirectory() const;
    [[nodiscard]] bool smartSelectionEnabled() const;

    bool requestCaptureHistoryPolicy(const CaptureHistoryPolicy& policy);
    [[nodiscard]] std::shared_future<StorageResult>
    requestCaptureHistoryPolicyAsync(const CaptureHistoryPolicy& policy);
    bool requestSmartSelection(bool enabled);
    [[nodiscard]] std::shared_future<StorageResult> requestSmartSelectionAsync(bool enabled);
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
    std::unique_ptr<StorageUsageTracker> m_usageTracker;
    bool m_initialized = false;
};
} // namespace snow_shot::storage

Q_DECLARE_METATYPE(snow_shot::storage::CaptureHistoryUsage)
Q_DECLARE_METATYPE(snow_shot::storage::StorageStatus)

#endif // SNOW_SHOT_STORAGE_APPLICATIONSTORAGE_H
