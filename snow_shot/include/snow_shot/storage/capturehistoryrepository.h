#ifndef SNOW_SHOT_STORAGE_CAPTUREHISTORYREPOSITORY_H
#define SNOW_SHOT_STORAGE_CAPTUREHISTORYREPOSITORY_H

#include "snow_shot/storage/capturehistorytypes.h"

#include <QDateTime>
#include <QString>
#include <QVector>

#include <functional>
#include <future>
#include <memory>
#include <optional>

namespace snow_shot::storage {
struct CaptureHistoryRepositoryCallbacks {
    std::function<void()> recordsChanged;
    std::function<void(const CaptureHistoryUsage&)> usageChanged;
    std::function<void(const QString&)> errorChanged;
    std::function<void(bool, const QString&)> policyFinished;
    std::function<void(bool, const QString&)> clearFinished;
};

struct CaptureHistoryRepositoryOptions {
    bool writeAvailable = true;
    CaptureHistoryPolicy policy;
    std::function<QDateTime()> clock;
    CaptureHistoryRepositoryCallbacks callbacks;
    int maxQueuedPublications = 2;
};

class CaptureHistoryRepository {
  public:
    virtual ~CaptureHistoryRepository() = default;

    [[nodiscard]] virtual QVector<CaptureHistoryRecord> records() const = 0;
    [[nodiscard]] virtual CaptureHistoryUsage usage() const = 0;
    [[nodiscard]] virtual CaptureHistoryPolicy policy() const = 0;
    [[nodiscard]] virtual std::shared_future<CaptureHistoryPublishResult>
    publish(CaptureHistoryDraft draft) = 0;
    [[nodiscard]] virtual std::optional<CaptureHistoryPayload>
    load(const CaptureHistoryRecord& record) const = 0;
    [[nodiscard]] virtual std::optional<CaptureHistoryAssetSet>
    displayAssets(const CaptureHistoryRecord& record) const = 0;
    [[nodiscard]] virtual std::optional<QImage>
    loadResultImage(const CaptureHistoryRecord& record) const = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult> remove(const QString& id) = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult>
    updatePolicy(CaptureHistoryPolicy policy) = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult> requestPrune() = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult> requestClear() = 0;
    virtual void drain() = 0;
    [[nodiscard]] virtual QString lastError() const = 0;
};

[[nodiscard]] std::unique_ptr<CaptureHistoryRepository>
makeCaptureHistoryRepository(QString configurationDirectory,
                             CaptureHistoryRepositoryOptions options = {});

[[nodiscard]] std::unique_ptr<CaptureHistoryRepository>
makeCaptureHistoryRepository(QString configurationDirectory, bool writeAvailable,
                             std::function<QDateTime()> clock = {});
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CAPTUREHISTORYREPOSITORY_H
