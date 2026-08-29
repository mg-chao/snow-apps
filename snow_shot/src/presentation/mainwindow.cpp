#include "snow_shot/presentation/mainwindow.h"

#include "snow_shot/platform/windows/windowchrome.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/components/titlebarwidget.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/settings/settingsruntimebindings.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMenuBar>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QAbstractButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr int MAIN_WINDOW_WIDTH = 900;
constexpr int MAIN_WINDOW_HEIGHT = 556;
constexpr int MAIN_WINDOW_MIN_WIDTH = 512;
constexpr int MAIN_WINDOW_MIN_HEIGHT = 316;
constexpr int TITLE_BAR_BOTTOM_SHADOW_HEIGHT = 6;
constexpr int TITLE_BAR_BOTTOM_SHADOW_ALPHA = 10;

class TitleBarBottomShadowWidget final : public QWidget {
  public:
    explicit TitleBarBottomShadowWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        (void)event;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        QLinearGradient shadowGradient(QPointF(0.0, 0.0), QPointF(0.0, height()));
        shadowGradient.setColorAt(0.0, QColor(0, 0, 0, TITLE_BAR_BOTTOM_SHADOW_ALPHA));
        shadowGradient.setColorAt(1.0, QColor(0, 0, 0, 0));

        painter.fillRect(rect(), Qt::transparent);
        painter.fillRect(rect(), shadowGradient);
    }
};
} // namespace

MainWindow::MainWindow(
    snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings, QWidget* parent)
    : QMainWindow(parent), m_runtimeBindings(runtimeBindings) {
    setObjectName(QStringLiteral("snowShotMainWindow"));
    setAccessibleName(QStringLiteral("SnowShot"));
    setWindowTitle(QStringLiteral("SnowShot"));
    resize(MAIN_WINDOW_WIDTH, MAIN_WINDOW_HEIGHT);
    setMinimumSize(MAIN_WINDOW_MIN_WIDTH, MAIN_WINDOW_MIN_HEIGHT);
    setMouseTracking(true);
    setAttribute(Qt::WA_DeleteOnClose);

    menuBar()->hide();
    statusBar()->hide();

    buildUi();
    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this](const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
                applyTheme(scheme);
            });
    applyTheme(themeManager.themeColorScheme());
}

bool MainWindow::event(QEvent* event) {
    const bool handled = QMainWindow::event(event);

#ifdef Q_OS_WIN
    // The DWM frame extension belongs to the HWND, which Qt recreates after close().
    if (event->type() == QEvent::WinIdChange && internalWinId() != 0) {
        setupDwmShadow();
    }
#endif

    return handled;
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);

    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme());
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    syncTitleBarBottomShadowGeometry();
}

void MainWindow::setupDwmShadow() {
#ifdef Q_OS_WIN
    snow_shot::platform::windows::setupDwmShadow(this);
#endif
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (snow_shot::platform::windows::handleNativeWindowEvent(m_titleBar, message, result)) {
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::buildUi() {
    const auto& settingsCatalog =
        snow_shot::presentation::settings::builtInSettingsCatalog();
    const auto metric =
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias;

    auto* root = new QWidget(this);
    root->setAutoFillBackground(true);
    setCentralWidget(root);

    m_titleBarBottomShadow = new TitleBarBottomShadowWidget(root);
    m_titleBarBottomShadow->hide();

    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* titleBar = new TitleBarWidget(metric, root);
    rootLayout->addWidget(titleBar, 0);

    connect(titleBar->minimizeButton(), &QAbstractButton::clicked, this, &QWidget::showMinimized);
    connect(titleBar->closeButton(), &QAbstractButton::clicked, this, &QWidget::close);
    m_titleBar = titleBar;

    auto* body = new QWidget(root);
    body->setAutoFillBackground(true);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto* sidebar = new SidebarWidget(settingsCatalog, body);
    bodyLayout->addWidget(sidebar, 0);
    m_sidebar = sidebar;

    auto* contentShell = new QWidget(body);
    contentShell->setAutoFillBackground(true);
    auto* contentShellLayout = new QVBoxLayout(contentShell);
    contentShellLayout->setContentsMargins(0, 0, 0, 0);
    contentShellLayout->setSpacing(0);

    auto* contentHeader = new MainContentHeaderWidget(settingsCatalog, metric, contentShell);
    contentShellLayout->addWidget(contentHeader, 0);
    m_contentHeader = contentHeader;

    auto* contentArea = new QWidget(contentShell);
    contentArea->setAutoFillBackground(true);
    auto* contentAreaLayout = new QVBoxLayout(contentArea);
    contentAreaLayout->setContentsMargins(metric.padding, metric.padding, metric.padding,
                                          metric.padding);
    contentAreaLayout->setSpacing(0);
    auto* contentCard = new ContentCardWidget(settingsCatalog, m_runtimeBindings, contentArea);
    contentAreaLayout->addWidget(contentCard, 1);
    m_contentCard = contentCard;
    contentShellLayout->addWidget(contentArea, 1);
    bodyLayout->addWidget(contentShell, 1);
    rootLayout->addWidget(body, 1);
    sidebar->raise();
    syncTitleBarBottomShadowGeometry();

    connect(m_sidebar, &SidebarWidget::routeSelected, m_contentCard,
            &ContentCardWidget::setCurrentRoute);
    connect(m_contentHeader, &MainContentHeaderWidget::sectionRequested, m_contentCard,
            &ContentCardWidget::activateSection);
    connect(m_contentHeader, &MainContentHeaderWidget::locationRequested, m_contentCard,
            &ContentCardWidget::navigateTo);
    connect(m_contentCard, &ContentCardWidget::routeChanged, m_sidebar,
            &SidebarWidget::setCurrentRoute);
    connect(m_contentCard, &ContentCardWidget::sectionListChanged, this, [this]() {
        if (m_contentHeader != nullptr && m_contentCard != nullptr) {
            m_contentHeader->setSections(m_contentCard->currentSections());
        }
    });
    connect(m_contentCard, &ContentCardWidget::locationChanged, this,
            [this](const snow_shot::presentation::settings::SettingsLocation& location) {
                if (m_contentHeader != nullptr) {
                    m_contentHeader->setCurrentSection(location.sectionId);
                }
            });
    connect(m_contentCard, &ContentCardWidget::screenshotRequested, this,
            &MainWindow::screenshotRequested);
    connect(m_contentCard, &ContentCardWidget::quickActionRequested, this,
            &MainWindow::quickActionRequested);
    connect(m_contentCard, &ContentCardWidget::screenshotHistoryEditRequested, this,
            &MainWindow::screenshotHistoryEditRequested);
    m_contentCard->setCurrentRoute(m_sidebar->currentRoute());
    m_contentHeader->setSections(m_contentCard->currentSections());
    m_contentHeader->setCurrentSection(m_contentCard->currentLocation().sectionId);
}

void MainWindow::showInterfaceSettings() {
    if (m_contentCard != nullptr) {
        m_contentCard->showInterfaceSettings();
    }

    showAndActivate();
}

void MainWindow::showScreenshotHistory() {
    showAndActivate();
    if (m_contentCard != nullptr) {
        m_contentCard->navigateTo({QStringLiteral("screenshot-history"), {}, {}});
    }
}

void MainWindow::showAndActivate() {
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
#ifdef Q_OS_WIN
    snow_shot::platform::windows::bringWindowToForeground(this);
#endif
}

void MainWindow::syncTitleBarBottomShadowGeometry() {
    if (m_titleBar == nullptr || m_titleBarBottomShadow == nullptr) {
        return;
    }

    QWidget* root = centralWidget();
    if (root == nullptr) {
        return;
    }

    const QPoint titleBarTopLeft = m_titleBar->mapTo(root, QPoint(0, 0));
    m_titleBarBottomShadow->setGeometry(0, titleBarTopLeft.y() + m_titleBar->height(),
                                        root->width(), TITLE_BAR_BOTTOM_SHADOW_HEIGHT);
    m_titleBarBottomShadow->show();
    m_titleBarBottomShadow->raise();
}

void MainWindow::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    if (m_isApplyingTheme) {
        return;
    }

    const QScopedValueRollback<bool> applyingThemeGuard(m_isApplyingTheme, true);

    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, scheme.map.colorBgLayout);
    palette.setColor(QPalette::Base, scheme.map.colorBgContainer);
    palette.setColor(QPalette::Button, scheme.map.colorBgContainer);
    palette.setColor(QPalette::ButtonText, scheme.map.colorText);
    palette.setColor(QPalette::WindowText, scheme.map.colorText);
    setPalette(palette);

    if (QWidget* centerWidget = centralWidget(); centerWidget != nullptr) {
        QPalette centerPalette = centerWidget->palette();
        centerPalette.setColor(QPalette::Window, scheme.map.colorBgLayout);
        centerWidget->setPalette(centerPalette);
    }

    if (m_titleBar != nullptr) {
        m_titleBar->applyTheme(scheme);
    }

    if (m_contentCard != nullptr) {
        m_contentCard->applyTheme(scheme);
    }

    if (m_contentHeader != nullptr) {
        m_contentHeader->applyTheme(scheme);
    }

    update();
}
