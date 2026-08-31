#ifndef SNOW_SHOT_PRESENTATION_MAINWINDOW_H
#define SNOW_SHOT_PRESENTATION_MAINWINDOW_H

#include <QByteArray>
#include <QMainWindow>

#include "snow_shot/presentation/globalshortcuttypes.h"

class QEvent;
class QResizeEvent;
class QWidget;
class SidebarWidget;
class ContentCardWidget;
class MainContentHeaderWidget;
class TitleBarWidget;
namespace snow_shot::presentation::styles {
struct ThemeColorScheme;
}
namespace snow_shot::presentation::settings {
class SettingsRegistry;
class SettingsRuntimeSession;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    MainWindow(const snow_shot::presentation::settings::SettingsRegistry& registry,
               snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
               QWidget* parent = nullptr);
    ~MainWindow() override = default;

    void showAndActivate();
    void showInterfaceSettings();
    void showScreenshotHistory();

  signals:
    void screenshotRequested();
    void quickActionRequested(snow_shot::presentation::GlobalShortcutAction action);
    void screenshotHistoryEditRequested(const QString& recordId);

  protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

  private:
    void buildUi();
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void syncTitleBarBottomShadowGeometry();
    void setupDwmShadow();
    TitleBarWidget* m_titleBar = nullptr;
    SidebarWidget* m_sidebar = nullptr;
    MainContentHeaderWidget* m_contentHeader = nullptr;
    ContentCardWidget* m_contentCard = nullptr;
    const snow_shot::presentation::settings::SettingsRegistry& m_settingsRegistry;
    snow_shot::presentation::settings::SettingsRuntimeSession& m_runtimeSession;
    QWidget* m_titleBarBottomShadow = nullptr;
    bool m_isApplyingTheme = false;
};

#endif // SNOW_SHOT_PRESENTATION_MAINWINDOW_H
