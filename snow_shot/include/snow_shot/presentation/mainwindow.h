#ifndef SNOW_SHOT_PRESENTATION_MAINWINDOW_H
#define SNOW_SHOT_PRESENTATION_MAINWINDOW_H

#include <QByteArray>
#include <QMainWindow>

#include "snow_shot/presentation/globalshortcutmanager.h"

class QEvent;
class QCloseEvent;
class QPaintEvent;
class QResizeEvent;
class QWidget;
class SidebarWidget;
class ContentCardWidget;
class MainContentHeaderWidget;
class ScreenshotController;
class TitleBarWidget;
namespace snow_shot::presentation::styles {
struct ThemeColorScheme;
}
namespace snow_shot::presentation {
class GlobalShortcutManager;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(ScreenshotController& screenshotController,
                        snow_shot::presentation::GlobalShortcutManager& globalShortcutManager,
                        QWidget* parent = nullptr);
    ~MainWindow() override = default;

    void showAndActivate();
    void showInterfaceSettings();
    void showScreenshotHistory();

  signals:
    void quickActionRequested(snow_shot::presentation::GlobalShortcutAction action);
    void firstFramePresented();
    // Emitted after the close event is accepted. ApplicationController owns the
    // window object and uses this notification to release the complete widget tree.
    void closed();

  protected:
    bool event(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

  private:
    void buildUi();
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme,
                    bool propagateToChildren = true);
    void syncTitleBarBottomShadowGeometry();
    void setupDwmShadow();
    TitleBarWidget* m_titleBar = nullptr;
    SidebarWidget* m_sidebar = nullptr;
    MainContentHeaderWidget* m_contentHeader = nullptr;
    ContentCardWidget* m_contentCard = nullptr;
    ScreenshotController* m_screenshotController = nullptr;
    snow_shot::presentation::GlobalShortcutManager* m_globalShortcutManager = nullptr;
    QWidget* m_titleBarBottomShadow = nullptr;
    bool m_isApplyingTheme = false;
    bool m_firstFramePresented = false;
};

#endif // SNOW_SHOT_PRESENTATION_MAINWINDOW_H
