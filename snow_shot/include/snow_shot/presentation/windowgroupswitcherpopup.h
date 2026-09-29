#pragma once
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "widgets/floating_surface.h"
#include <memory>
class QScreen;
namespace snow_shot::presentation {
class WindowGroupSwitcherPopup final : public adqt::widgets::AdFloatingSurface {
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
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
