#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWREPOSITORY_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWREPOSITORY_H

#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/storageresult.h"

#include <QVector>

#include <QDateTime>

#include <memory>
#include <optional>

namespace snow_shot::storage {

struct PinnedWindowSummary final {
    QString id;
    QString groupId = QStringLiteral("default");
    QDateTime updatedUtc;
};

class PinnedWindowRepository final {
  public:
    explicit PinnedWindowRepository(QString configurationDirectory, bool writeAvailable = true,
                                    int debounceMilliseconds = 1000);
    ~PinnedWindowRepository();

    [[nodiscard]] static constexpr int maximumGroupCount() {
        return 128;
    }

    [[nodiscard]] QVector<PinnedWindowRecord> records() const;
    [[nodiscard]] std::optional<PinnedWindowRecord> loadRecord(const QString& id) const;
    [[nodiscard]] QVector<PinnedWindowSummary> summaries() const;
    [[nodiscard]] quint64 revision() const;
    [[nodiscard]] QVector<PinnedWindowGroup> groups() const;
    [[nodiscard]] QString activeGroupId() const;
    [[nodiscard]] StorageResult setActiveGroup(const QString& groupId);
    [[nodiscard]] StorageResult setGroups(QVector<PinnedWindowGroup> groups,
                                          const QString& activeGroupId);
    [[nodiscard]] StorageResult removeEmptyGroups();
    [[nodiscard]] StorageResult setRecordGroup(const QString& recordId,
                                                const QString& groupId);
    [[nodiscard]] StorageResult upsert(PinnedWindowRecord record);
    [[nodiscard]] StorageResult remove(const QString& id);
    [[nodiscard]] StorageResult flush();
    [[nodiscard]] QString lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PINNEDWINDOWREPOSITORY_H
