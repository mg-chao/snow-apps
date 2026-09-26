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
struct CaptureHistorySnapshot {
    quint64 revision = 0;
    QVector<CaptureHistoryRecord> records;
};
enum class CaptureHistoryOperation { IndexRead, IndexWrite, PayloadRead, WorkerStarted };

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
    // Optional diagnostics hook; called on the thread performing the operation.
    std::function<void(CaptureHistoryOperation)> operationObserved;
};

class CaptureHistoryRepository {
  public:
    virtual ~CaptureHistoryRepository() = default;

    [[nodiscard]] virtual QVector<CaptureHistoryRecord> records() const = 0;
    [[nodiscard]] virtual CaptureHistorySnapshot recordsSnapshot() const {
        return {0, records()};
    }
    [[nodiscard]] virtual std::shared_future<StorageResult>
    removeIfRevision(QVector<QString> ids, quint64 expectedRevision, bool clear = false) {
        Q_UNUSED(ids);
        Q_UNUSED(expectedRevision);
        Q_UNUSED(clear);
        std::promise<StorageResult> promise;
        promise.set_value(StorageResult::failure(QStringLiteral("unsupported")));
        return promise.get_future().share();
    }
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
    [[nodiscard]] virtual std::optional<PreparedPngImage>
    loadResultPng(const CaptureHistoryRecord& record) const = 0;
    virtual void reportReadFailure(const CaptureHistoryRecord& record, const QString& reason) = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult> remove(const QString& id) = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult> removeMany(QVector<QString> ids) = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult>
    updatePolicy(CaptureHistoryPolicy policy) = 0;
    [[nodiscard]] virtual std::shared_future<StorageResult> requestClear() = 0;
    virtual void drain() = 0;
    [[nodiscard]] virtual QString lastError() const = 0;
};

[[nodiscard]] std::unique_ptr<CaptureHistoryRepository>
makeCaptureHistoryRepository(QString configurationDirectory,
                             CaptureHistoryRepositoryOptions options = {});
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CAPTUREHISTORYREPOSITORY_H
