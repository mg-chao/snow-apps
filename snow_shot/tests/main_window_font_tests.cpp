#include "snow_shot/presentation/components/actionrow.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QLabel>
#include <QMenu>
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

void mainWindowTitlesKeepSmoothRendering() {
    const QFont applicationFont = QApplication::font();
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    window.show();
    flushEvents();
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
    mainWindowTitlesKeepSmoothRendering();
    storage.shutdown();
    return 0;
}
