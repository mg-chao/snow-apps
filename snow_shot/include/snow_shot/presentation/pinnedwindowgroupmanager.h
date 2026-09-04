#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWGROUPMANAGER_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWGROUPMANAGER_H

#include "snow_shot/storage/pinnedwindowtypes.h"

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <optional>

class QWidget;
class ScreenshotPinnedWindow;

namespace snow_shot::storage {
class PinnedWindowRepository;
}

namespace snow_shot::presentation {
class PinnedWindowGroupManager final : public QObject {
    Q_OBJECT

  public:
    explicit PinnedWindowGroupManager(storage::PinnedWindowRepository* repository = nullptr,
                                      QObject* parent = nullptr);

    [[nodiscard]] QVector<storage::PinnedWindowGroup> groups() const;
    [[nodiscard]] QString activeGroupId() const;
    [[nodiscard]] QString displayName(const QString& groupId) const;
    [[nodiscard]] bool contains(const QString& groupId) const;
    [[nodiscard]] int windowCount(const QString& groupId) const;
    [[nodiscard]] bool hasWindow(const QString& persistenceId) const;

    bool setActiveGroup(const QString& groupId);
    [[nodiscard]] std::optional<QString> createGroup(const QString& name,
                                                      ::ScreenshotPinnedWindow* currentWindow = nullptr);
    bool deleteEmptyGroups();
    bool moveWindow(::ScreenshotPinnedWindow* window, const QString& groupId);
    void restoreActiveGroupWindows();

    void registerWindow(::ScreenshotPinnedWindow* window, const QString& groupId);
    void unregisterWindow(::ScreenshotPinnedWindow* window);
    void openCreateGroupModal(QWidget* owner,
                              ::ScreenshotPinnedWindow* currentWindow = nullptr);

  signals:
    void groupsChanged();
    void activeGroupChanged(const QString& groupId);
    void restoreActiveGroupWindowsRequested();

  private:
    [[nodiscard]] QString normalizedDisplayName(const storage::PinnedWindowGroup& group) const;
    [[nodiscard]] QString windowKey(::ScreenshotPinnedWindow* window) const;
    [[nodiscard]] bool persist();
    [[nodiscard]] QString uniqueGeneratedName() const;

    storage::PinnedWindowRepository* m_repository = nullptr;
    QVector<storage::PinnedWindowGroup> m_groups;
    QString m_activeGroupId;
    QHash<QString, QPointer<::ScreenshotPinnedWindow>> m_windows;
    QSet<QString> m_inactiveClosing;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_PINNEDWINDOWGROUPMANAGER_H
