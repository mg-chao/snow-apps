#pragma once
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <memory>
class QScreen;
namespace snow_shot::presentation {
class GlobalShortcutManager;
class WindowGroupSwitcherPopup;
class WindowGroupSwitcherController final : public QObject {
    Q_OBJECT
  public:
    WindowGroupSwitcherController(GlobalShortcutManager& shortcuts,
                                  PinnedWindowGroupManager& groups, QObject* parent = nullptr);
    ~WindowGroupSwitcherController() override;
    void openPicker();
    void activateShortcut(int registrationId);
    [[nodiscard]] bool isVisible() const;
    [[nodiscard]] QString selectedGroupId() const;
    [[nodiscard]] bool isTrackingInput() const;
    [[nodiscard]] WindowGroupSwitcherPopup* popup() const;
  public slots:
    void pollInput();
    void cancel();
    void selectGroup(const QString& id);
  signals:
    void errorOccurred(const QString& message);

  private:
    void begin(bool shortcutMode);
    void advance();
    void refreshGroups();
    void finish(const QString& commitId = {});
    GlobalShortcutManager& m_shortcuts;
    PinnedWindowGroupManager& m_groups;
    std::unique_ptr<WindowGroupSwitcherPopup> m_popup;
    QTimer m_timer;
    QPointer<QScreen> m_screen;
    QVector<WindowGroupDisplayEntry> m_entries;
    QString m_selectedId;
    QSet<int> m_participating;
    bool m_draining = false;
    bool m_escapeDown = false;
};
} // namespace snow_shot::presentation
