#ifndef SNOW_SHOT_PRESENTATION_SYSTEMTRAYCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SYSTEMTRAYCONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <memory>

#include "snow_shot/presentation/globalshortcuttypes.h"
#include "snow_shot/platform/systemnotification.h"

namespace snow_shot::presentation::settings {
struct TrayCommandManifest;
}
namespace adqt::widgets {
class AdContextMenu;
}

namespace snow_shot::presentation {
class PinnedWindowGroupManager;
class SystemTrayController final : public QObject {
    Q_OBJECT

  public:
    explicit SystemTrayController(QObject* parent = nullptr);
    SystemTrayController(const settings::TrayCommandManifest& manifest, QObject* parent = nullptr);
    // Keep the historical `(manifest, nullptr)` construction unambiguous now
    // that the manager-injection overload also accepts a nullable pointer.
    SystemTrayController(const settings::TrayCommandManifest& manifest, std::nullptr_t parent);
    SystemTrayController(const settings::TrayCommandManifest& manifest,
                         PinnedWindowGroupManager* groupManager, QObject* parent = nullptr);
    void setGroupManager(PinnedWindowGroupManager* groupManager);
    // Creates a fresh popup session; the menu retires itself after hiding.
    adqt::widgets::AdContextMenu* createContextMenu();
    ~SystemTrayController() override;

    void show();
    void hide();
    void showCaptureMessage(const QString& message, bool warning);
    void showWarningMessage(const QString& title, const QString& message);
    void showUpdateMessage(const QString& message);
    void showRecordingExportMessage(const QString& path);
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const;
    void setIconSelection(const QString& selection);
    [[nodiscard]] QString iconSelection() const;
    void setCustomIconPath(const QString& path);
    [[nodiscard]] QString customIconPath() const;
    void setLeftClickAction(const QString& action);
    [[nodiscard]] QString leftClickAction() const;
    [[nodiscard]] QString middleClickAction() const;
    void setMiddleClickAction(const QString& action);
    void setScreenshotDelaySeconds(int seconds);
    [[nodiscard]] int screenshotDelaySeconds() const;
    void setGlobalShortcuts(GlobalShortcutAction action,
                            const shortcuts::ShortcutBindingList& shortcuts);
    void setMenuOptions(const QStringList& options);
    [[nodiscard]] QStringList menuOptions() const;
    void setQuickActionChecked(GlobalShortcutAction action, bool checked);

  signals:
    void notificationDeliveryFinished(const snow_shot::platform::SystemNotificationRequest& request,
                                      const snow_shot::platform::SystemNotificationResult& result);
    void screenshotRequested();
    void showMainWindowRequested();
    void restartRequested();
    void openFunctionSettingsRequested();
    void openAboutRequested();
    void openRecordingFileRequested(const QString& path);
    void quickActionRequested(snow_shot::presentation::GlobalShortcutAction action);
    void exitRequested();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SYSTEMTRAYCONTROLLER_H
