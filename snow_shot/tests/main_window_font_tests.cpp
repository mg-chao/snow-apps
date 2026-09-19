#include "snow_shot/presentation/components/actionrow.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/titlebarwidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/message.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QTemporaryDir>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

void requireSmoothTitles(QWidget& container, int expectedSize) {
    int titleCount = 0;
    for (const auto* label : container.findChildren<QLabel*>()) {
        if (label->text().isEmpty() || label->font().pixelSize() != expectedSize) {
            continue;
        }
        ++titleCount;
        require(label->font().hintingPreference() == QFont::PreferNoHinting,
                "large titles must inherit the main window's unhinted outline rendering");
    }
    require(titleCount > 0, "exercise actual large title labels, not just the window font");
}

void customTitleBarUsesPlatformWindowControls() {
    const auto scheme = styles::ThemeManager::instance().themeColorScheme();
    QWidget host;
    host.resize(360, 80);
    TitleBarWidget titleBar(scheme.metricAlias, &host);
    titleBar.resize(host.width(), titleBar.height());
    host.show();
    flushEvents();

    auto* closeButton = titleBar.findChild<QAbstractButton*>(QStringLiteral("closeWindowButton"));
    auto* minimizeButton =
        titleBar.findChild<QAbstractButton*>(QStringLiteral("minimizeWindowButton"));
    auto* maximizeButton =
        titleBar.findChild<QAbstractButton*>(QStringLiteral("maximizeWindowButton"));
#ifdef Q_OS_MACOS
    require(closeButton == nullptr && minimizeButton == nullptr && maximizeButton == nullptr,
            "macOS must leave traffic-light rendering and interaction to AppKit");
#else
    require(closeButton != nullptr && minimizeButton != nullptr && maximizeButton != nullptr,
            "the custom title bar must own close, minimize, and maximize controls");
    require(minimizeButton->x() < maximizeButton->x() && maximizeButton->x() < closeButton->x(),
            "caption buttons must keep the Windows minimize/maximize/close order");
    require(minimizeButton->x() >= titleBar.width() / 2,
            "Windows caption buttons must stay right-aligned in the custom title bar");

    const QString maximizeText = maximizeButton->accessibleName();
    titleBar.setMaximized(true);
    require(!maximizeButton->accessibleName().isEmpty() &&
                maximizeButton->accessibleName() != maximizeText,
            "the maximize control must expose its restore action while maximized");
    titleBar.setMaximized(false);
    require(maximizeButton->accessibleName() == maximizeText,
            "the maximize control must restore its maximize action in the normal state");
#endif
}

void mainWindowTitlesKeepSmoothRendering() {
    const QFont applicationFont = QApplication::font();
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    window.show();
    flushEvents();
#ifdef Q_OS_MACOS
    auto* titleBar = window.findChild<TitleBarWidget*>();
    require(titleBar != nullptr,
            "macOS must place native traffic lights over SnowShot's in-content title bar");
    require(window.windowFlags().testFlag(Qt::ExpandedClientAreaHint) &&
                window.windowFlags().testFlag(Qt::NoTitleBarBackgroundHint) &&
                window.testAttribute(Qt::WA_LayoutOnEntireRect),
            "macOS must expand SnowShot's layout into the transparent native title-bar area");
    require(window.findChild<QWidget*>(QStringLiteral("titleBarBottomShadow")) != nullptr,
            "macOS must retain SnowShot's custom title-bar separator shadow");
    require(titleBar->closeButton() == nullptr && titleBar->minimizeButton() == nullptr &&
                titleBar->maximizeButton() == nullptr,
            "macOS must not render duplicate custom traffic-light controls");
#else
    auto* titleBar = window.findChild<TitleBarWidget*>();
    require(titleBar != nullptr, "non-macOS windows must retain the existing custom title bar");
    require(titleBar->maximizeButton() != nullptr,
            "the custom main-window title bar must expose a maximize control");
    titleBar->maximizeButton()->click();
    flushEvents();
    require(window.isMaximized(), "the maximize title-bar control must maximize the main window");
    titleBar->maximizeButton()->click();
    flushEvents();
    require(!window.isMaximized(), "the maximize title-bar control must restore the main window");
#endif
    auto* card = window.findChild<ContentCardWidget*>();
    require(card != nullptr, "main window content exists");
    const QString shortcutRoute = card->currentRoute();
    require(!card->findChildren<ActionRow*>().isEmpty(), "default route contains shortcut buttons");

    for (auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeManager::instance().setThemeAppearance(appearance);
        for (bool settingsPage : {false, true, false}) {
            if (settingsPage) {
                window.showInterfaceSettings();
            } else {
                card->setCurrentRoute(shortcutRoute);
            }
            flushEvents();
            const int titleSize =
                styles::ThemeManager::instance().themeColorScheme().metricAlias.fontSizeLG;
            requireSmoothTitles(*card, titleSize);
            QEvent languageChange(QEvent::LanguageChange);
            QApplication::sendEvent(&window, &languageChange);
            QEvent dpiChange(QEvent::DevicePixelRatioChange);
            QApplication::sendEvent(&window, &dpiChange);
            flushEvents();
            requireSmoothTitles(*card, titleSize);
        }
    }
    require(QApplication::font() == applicationFont,
            "main window typography must not change the application font for other windows");
}

#ifdef Q_OS_MACOS
void permissionRedirectShowsMainInterfacePrompt() {
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    auto* messages = adqt::widgets::AdMessageService::instance(&window);
    QPointer<adqt::widgets::AdMessageHandle> prompt;
    QObject::connect(messages, &adqt::widgets::AdMessage::messageOpened, &window,
                     [&prompt](adqt::widgets::AdMessageHandle* handle) { prompt = handle; });

    window.showAppPermissions(QStringLiteral("accessibility"));
    flushEvents();
    auto* card = window.findChild<ContentCardWidget*>();
    require(window.isVisible() && card != nullptr &&
                card->currentLocation().pageId == QStringLiteral("app-permissions") &&
                card->currentLocation().itemId == QStringLiteral("accessibility"),
            "permission failure must open the main interface at the affected permission");
    require(prompt != nullptr && messages->count() == 1 &&
                prompt->key() == QStringLiteral("main-app-permission-required") &&
                prompt->type() == adqt::widgets::AdMessage::Type::Warning &&
                prompt->content() == QStringLiteral("Grant the required permission to continue"),
            "permission failure must show a warning through the main-interface message component");

    window.showAppPermissions(QStringLiteral("screen-recording"));
    flushEvents();
    require(messages->count() == 1 &&
                card->currentLocation().itemId == QStringLiteral("screen-recording"),
            "repeated permission failures must update one prompt while navigating to the latest "
            "permission");
}
#endif

void applicationTypographyCoversUnownedSurfaces() {
    // The theme owns application-wide typography: tooltips, message boxes, native menus,
    // and ownerless overlay-style windows all resolve unhinted outlines at fractional DPI.
    require(QApplication::font().hintingPreference() == QFont::PreferNoHinting,
            "the themed application font must render unhinted outlines");
    for (const char* popupClass : {"QTipLabel", "QMessageBox", "QMenu"}) {
        const QFont popupFont = QApplication::font(popupClass);
        require(popupFont.hintingPreference() == QFont::PreferNoHinting,
                "native popup class fonts must render unhinted outlines");
    }
    QWidget standalone; // models overlay, palette, pinned, and recognition windows
    require(standalone.font().hintingPreference() == QFont::PreferNoHinting,
            "parentless top-level widgets must inherit unhinted outlines");
    QMenu trayMenu; // seeds from the QMenu class font instead of an owner chain
    require(trayMenu.font().hintingPreference() == QFont::PreferNoHinting,
            "native menus must render unhinted outlines");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc")));
    QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf")));
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("main_window_font_tests"));
    QTemporaryDir directory;
    require(directory.isValid(), "isolated font test storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 8000}).success,
            "initialize isolated font test storage");
    styles::ThemeManager::instance().initialize(application);
    applicationTypographyCoversUnownedSurfaces();
    customTitleBarUsesPlatformWindowControls();
    mainWindowTitlesKeepSmoothRendering();
#ifdef Q_OS_MACOS
    permissionRedirectShowsMainInterfacePrompt();
#endif
    storage.shutdown();
    return 0;
}
