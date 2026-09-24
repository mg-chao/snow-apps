#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWGROUPMANAGER_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWGROUPMANAGER_H

#include "snow_shot/storage/pinnedwindowtypes.h"

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <optional>
#include <limits>

class QWidget;
class ScreenshotPinnedWindow;

namespace snow_shot::storage {
class PinnedWindowRepository;
}

namespace snow_shot::presentation {
struct GroupWindowCounts final {
    int nonIgnored = 0;
    int total = 0;
};

class PinnedWindowGroupManager final : public QObject {
    Q_OBJECT

  public:
    explicit PinnedWindowGroupManager(storage::PinnedWindowRepository* repository = nullptr,
                                      QObject* parent = nullptr);

    [[nodiscard]] QVector<storage::PinnedWindowGroup> groups() const;
    [[nodiscard]] QVector<storage::PinnedWindowGroup> groupsSortedForDisplay() const;
    [[nodiscard]] QString activeGroupId() const;
    [[nodiscard]] QString displayName(const QString& groupId) const;
    [[nodiscard]] bool contains(const QString& groupId) const;
    [[nodiscard]] GroupWindowCounts windowCounts(const QString& groupId) const;
    [[nodiscard]] int windowCount(const QString& groupId) const;
    [[nodiscard]] bool hasWindow(const QString& persistenceId) const;
    void onPinnedRecordsChanged();

    bool setActiveGroup(const QString& groupId);
    [[nodiscard]] std::optional<QString>
    createGroup(const QString& name, ::ScreenshotPinnedWindow* currentWindow = nullptr);
    bool deleteEmptyGroups();
    bool deleteSpecifiedGroup(const QString& groupId);
    bool moveWindow(::ScreenshotPinnedWindow* window, const QString& groupId);
    void restoreActiveGroupWindows();
    bool showWindow(const QString& id);
    void destroyWindow(const QString& id);
    void markWindowClosing(::ScreenshotPinnedWindow* window);

    void registerWindow(::ScreenshotPinnedWindow* window, const QString& groupId);
    void unregisterWindow(::ScreenshotPinnedWindow* window);
    void registerPendingPin(const QString& persistenceId, const QString& groupId);
    void completePendingPin(const QString& persistenceId);
    void openCreateGroupModal(QWidget* owner, ::ScreenshotPinnedWindow* currentWindow = nullptr);
    void openDeleteEmptyGroupsConfirmation(QWidget* owner);
    void openDeleteSpecifiedGroupConfirmation(const QString& groupId, QWidget* owner);

  signals:
    void groupsChanged();
    void activeGroupChanged(const QString& groupId);
    void restoreActiveGroupWindowsRequested();
    void groupDeletionRequested(const QString& groupId);

  private:
    [[nodiscard]] QString normalizedDisplayName(const storage::PinnedWindowGroup& group) const;
    [[nodiscard]] QString windowKey(::ScreenshotPinnedWindow* window) const;
    [[nodiscard]] bool persist();
    [[nodiscard]] QString uniqueGeneratedName() const;
    void scheduleGroupsChanged();

    storage::PinnedWindowRepository* m_repository = nullptr;
    QVector<storage::PinnedWindowGroup> m_groups;
    QString m_activeGroupId;
    QHash<QString, QPointer<::ScreenshotPinnedWindow>> m_windows;
    QSet<QString> m_inactiveClosing;
    QHash<QString, QString> m_pendingGroups;
    mutable quint64 m_countsRevision = (std::numeric_limits<quint64>::max)();
    mutable QHash<QString, int> m_persistedCounts;
    mutable QHash<QString, int> m_persistedTotalCounts;
    mutable QHash<QString, QSet<QString>> m_persistedIdsByGroup;
    mutable QHash<QString, QSet<QString>> m_allPersistedIdsByGroup;
    bool m_groupsChangedScheduled = false;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_PINNEDWINDOWGROUPMANAGER_H
