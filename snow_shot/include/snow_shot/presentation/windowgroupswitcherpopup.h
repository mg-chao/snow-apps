#pragma once
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include <QWidget>
#include <memory>
class QScreen;
namespace snow_shot::presentation {
class WindowGroupSwitcherPopup final : public QWidget {
    Q_OBJECT
  public:
    explicit WindowGroupSwitcherPopup();
    ~WindowGroupSwitcherPopup() override;
    void setGroups(QVector<WindowGroupDisplayEntry> groups, const QString& activeId);
    void setSelectedGroup(const QString& id);
    void setShortcutMode(bool enabled);
    void showOnScreen(QScreen* screen);
    void updatePlacement();
  signals:
    void groupClicked(const QString& id);
    void languageChanged();

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
