#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWREPOSITORY_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWREPOSITORY_H

#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/preparedpngimage.h"
#include "snow_shot/storage/storageresult.h"

#include <QVector>

#include <QDateTime>

#include <memory>
#include <mutex>
#include <functional>
#include <optional>

namespace snow_shot::storage {

struct PinnedWindowSummary final {
    QString id;
    QString groupId = QStringLiteral("default");
    QDateTime updatedUtc;
    PinnedWindowCreationSource creationSource = PinnedWindowCreationSource::Other;
    QDateTime createdUtc;
    QDateTime lastClosedUtc;
    bool ignored = false;
    quint64 activitySequence = 0;
    [[nodiscard]] QDateTime activityUtc() const {
        return lastClosedUtc.isValid() ? lastClosedUtc : createdUtc;
    }
};

struct PinnedWindowPreviewSource final {
    PinnedWindowSourceKind sourceKind = PinnedWindowSourceKind::ImageData;
    QImage image;
    QString originalHtml;
    QString originalText;
    double firstCreationTextDpi = 1.0;
};

enum class PinnedWindowOperation {
    PayloadReadAdmission,
    PayloadRead,
    PayloadWrite,
    RetentionWait,
    RetentionScan,
    StorageReadDrain
};

class PinnedWindowRepository final {
  public:
    // Optional diagnostics hook runs on the operation's thread, outside metadata
    // locks. Hooks must not reenter payload reads, flush, retention, or storage
    // suspension because payload operations keep their file lease while the hook runs.
    explicit PinnedWindowRepository(
        QString configurationDirectory, bool writeAvailable = true, int debounceMilliseconds = 1000,
        std::function<void(PinnedWindowOperation)> operationObserved = {});
    ~PinnedWindowRepository();

    [[nodiscard]] static constexpr int maximumGroupCount() {
        return 128;
    }

    // Allocation admission runs while the payload revision is leased; the
    // callback must not start another payload read, flush, retention, or storage suspension.
    [[nodiscard]] std::optional<PinnedWindowRecord>
    loadRecord(const QString& id, std::function<bool(qint64)> allocationCheck = {}) const;
    [[nodiscard]] std::optional<PinnedWindowPreviewSource>
    loadPreviewSource(const QString& id) const;
    [[nodiscard]] std::optional<quint64> previewSourceRevision(const QString& id) const;
    [[nodiscard]] QVector<PinnedWindowSummary> summaries() const;
    // Reads source identity without materializing any persisted image payload.
    [[nodiscard]] PinnedSourceIdentity sourceIdentity(const QString& id) const;
    [[nodiscard]] quint64 revision() const;
    // Advances only when records enter, leave, close, restore, or change groups.
    [[nodiscard]] quint64 membershipRevision() const;
    [[nodiscard]] int allocateHideToTopAccent();
    [[nodiscard]] QVector<PinnedWindowGroup> groups() const;
    [[nodiscard]] QString activeGroupId() const;
    [[nodiscard]] StorageResult setActiveGroup(const QString& groupId);
    [[nodiscard]] StorageResult setGroups(QVector<PinnedWindowGroup> groups,
                                          const QString& activeGroupId);
    [[nodiscard]] StorageResult setRecordGroup(const QString& recordId, const QString& groupId);
    [[nodiscard]] StorageResult removeEmptyGroup(const QString& groupId);
    // The built-in Default group is cleared but never removed.
    [[nodiscard]] StorageResult removeGroupAndRecords(const QString& groupId);
    [[nodiscard]] StorageResult create(PinnedWindowRecord record, PreparedPngImage sourceImage);
    [[nodiscard]] StorageResult create(PinnedWindowRecord record);
    // First saves from asynchronous pin creation require a live reservation.
    [[nodiscard]] StorageResult createReserved(PinnedWindowRecord record,
                                               PreparedPngImage sourceImage);
    [[nodiscard]] StorageResult createReserved(PinnedWindowRecord record);
    [[nodiscard]] StorageResult updateState(PinnedWindowRecord record);
    [[nodiscard]] StorageResult upsert(PinnedWindowRecord record);
    // A late state save must not recreate a record removed by the user or retention.
    [[nodiscard]] StorageResult upsertExisting(PinnedWindowRecord record);
    [[nodiscard]] StorageResult remove(const QString& id);
    [[nodiscard]] StorageResult removeMany(const QVector<QString>& ids);
    // Callbacks run under the repository lock; dispatch notifications without reentering it.
    void setChangedCallback(std::function<void()> callback);
    void reserveCreation(const QString& id, QDateTime when = QDateTime::currentDateTimeUtc());
    // Releases a failed or canceled first save, including any close that preceded it.
    void cancelCreation(const QString& id);
    [[nodiscard]] StorageResult markClosed(const QString& id,
                                           QDateTime when = QDateTime::currentDateTimeUtc());
    // Updates close state immediately; the caller must request retention cleanup separately.
    [[nodiscard]] StorageResult
    markClosedDeferred(const QString& id, QDateTime when = QDateTime::currentDateTimeUtc());
    [[nodiscard]] StorageResult beginRestore(const QString& id);
    void cancelRestore(const QString& id);
    [[nodiscard]] StorageResult markRestored(const QString& id);
    // Application startup/settings can defer the potentially slow cleanup to a worker.
    [[nodiscard]] StorageResult setPolicy(PinnedWindowPolicy policy,
                                          bool enforceImmediately = true);
    [[nodiscard]] PinnedWindowPolicy policy() const;
    void setCompressionLevel(const QString& level);
    [[nodiscard]] StorageResult enforcePolicy(QDateTime now = QDateTime::currentDateTimeUtc());
    [[nodiscard]] StorageResult clearClosed();
    [[nodiscard]] StorageResult flush();
    // Suspension rejects new payload reads and drains admitted readers before
    // migration may remove this generation's backing files.
    void suspendWrites(bool suspended);
    // Called with writers suspended, after flush, and with a prepared destination.
    void exchangeStorage(PinnedWindowRepository& prepared);
    [[nodiscard]] QString lastError() const;

  private:
    [[nodiscard]] StorageResult createImpl(PinnedWindowRecord record, PreparedPngImage sourceImage,
                                           bool requireReservation);
    [[nodiscard]] StorageResult createImpl(PinnedWindowRecord record, bool requireReservation);
    [[nodiscard]] StorageResult upsertImpl(PinnedWindowRecord record, bool requireExisting);
    [[nodiscard]] StorageResult markClosedImpl(const QString& id, QDateTime when,
                                               bool enforceImmediately);
    mutable std::recursive_mutex m_accessMutex;
    bool m_suspended = false;
    struct Impl;
    // Readers retain the admitted storage generation across migration.
    std::shared_ptr<Impl> m_impl;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PINNEDWINDOWREPOSITORY_H
