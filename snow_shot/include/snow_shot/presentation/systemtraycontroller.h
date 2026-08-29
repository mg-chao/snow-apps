#ifndef SNOW_SHOT_PRESENTATION_SYSTEMTRAYCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SYSTEMTRAYCONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

#include "snow_shot/presentation/globalshortcuttypes.h"

namespace snow_shot::presentation::settings {
class SettingsCatalog;
}

namespace snow_shot::presentation {
class SystemTrayController final : public QObject {
    Q_OBJECT

  public:
    explicit SystemTrayController(QObject* parent = nullptr);
    SystemTrayController(const settings::SettingsCatalog& catalog, QObject* parent = nullptr);
    ~SystemTrayController() override;

    void show();
    void hide();
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const;
    void setIconSelection(const QString& selection);
    [[nodiscard]] QString iconSelection() const;
    void setCustomIconPath(const QString& path);
    [[nodiscard]] QString customIconPath() const;
    void setLeftClickAction(const QString& action);
    [[nodiscard]] QString leftClickAction() const;
    void setScreenshotDelaySeconds(int seconds);
    [[nodiscard]] int screenshotDelaySeconds() const;
    void setGlobalShortcuts(GlobalShortcutAction action, const QStringList& shortcuts);
    void setMenuOptions(const QStringList& options);
    [[nodiscard]] QStringList menuOptions() const;
    [[nodiscard]] bool shortcutFunctionsDisabled() const;

  signals:
    void screenshotRequested();
    void showMainWindowRequested();
    void quickActionRequested(snow_shot::presentation::GlobalShortcutAction action);
    void shortcutFunctionsDisabledChanged(bool disabled);
    void exitRequested();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SYSTEMTRAYCONTROLLER_H
