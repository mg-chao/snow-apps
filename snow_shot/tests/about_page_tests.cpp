#include "snow_shot/presentation/components/aboutpagewidget.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/button.h"
#include "widgets/descriptions.h"
#include "widgets/navigation_menu.h"
#include "widgets/scroll_area.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QPointer>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

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

template <typename T> T* child(QObject& parent, const char* name) {
    auto* result = parent.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, name);
    return result;
}

void snapshot(QWidget& widget, const QString& name) {
    const QString directory = qEnvironmentVariable("SNOW_SHOT_ABOUT_SNAPSHOT_DIR");
    if (!directory.isEmpty()) {
        require(QDir().mkpath(directory), "create About preview directory");
        require(widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))),
                "save About preview");
    }
}

void versionIsExactSelectableAndCopyable() {
    const QString version = QStringLiteral("12.34.56-beta.7+build.89");
    QCoreApplication::setApplicationVersion(version);
    AboutPageWidget page;
    auto* value = child<QLabel>(page, "aboutVersionValue");
    auto* copy = child<adqt::widgets::AdButton>(page, "aboutCopyVersion");
    require(value->text() == version, "display the complete version including prerelease metadata");
    require(value->accessibleName().contains(version), "screen readers can identify the version");
    require(value->textFormat() == Qt::PlainText &&
                value->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard) &&
                value->textInteractionFlags().testFlag(Qt::TextSelectableByMouse),
            "version is plain text and selectable with mouse and keyboard");
    require(copy->isEnabled() && copy->focusPolicy() == Qt::StrongFocus,
            "copy action is keyboard accessible");
    copy->click();
    require(QApplication::clipboard()->text() == version, "copy the exact displayed version");
    require(copy->text() == QStringLiteral("Copied"), "show copy feedback");
    auto* timer = child<QTimer>(page, "aboutCopyFeedbackTimer");
    require(timer != nullptr && timer->isActive(), "copy feedback expires");
    timer->stop();
    require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "simulate feedback timeout without waiting");
    require(copy->text() == QStringLiteral("Copy version"),
            "restore the copy action after feedback");
}

void absentVersionDoesNotInventARelease() {
    for (const QString& version : {QString(), QStringLiteral("   ")}) {
        QCoreApplication::setApplicationVersion(version);
        AboutPageWidget page;
        require(child<QLabel>(page, "aboutVersionValue")->text() == QStringLiteral("Unavailable"),
                "missing version uses a translated fallback");
        require(!child<adqt::widgets::AdButton>(page, "aboutCopyVersion")->isEnabled(),
                "missing version cannot be copied");
    }
}

void mainNavigationSearchThemesAndLanguages() {
    QCoreApplication::setApplicationVersion(QStringLiteral(SNOW_SHOT_TEST_VERSION));
    const auto& registry = settings::builtInSettingsRegistry();
    require(registry.isValid(), "About preserves catalog validity");
    const auto* definition = registry.catalog().pageForRoute(QStringLiteral("/about"));
    require(definition != nullptr && definition->kind == settings::SettingsPageKind::About &&
                definition->sections.isEmpty(),
            "About is a dedicated page without settings sections");
    const auto resolved = registry.catalog().resolveLocation(
        {QStringLiteral("about"), QStringLiteral("stale-section"), QStringLiteral("stale-item")});
    require(resolved.pageId == QStringLiteral("about") && resolved.sectionId.isEmpty() &&
                resolved.itemId.isEmpty(),
            "About discards stale section and item locations");

    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    window.show();
    flushEvents();
    auto* sidebar = window.findChild<SidebarWidget*>();
    auto* card = window.findChild<ContentCardWidget*>();
    auto* header = window.findChild<MainContentHeaderWidget*>();
    require(sidebar != nullptr && card != nullptr && header != nullptr,
            "main interface components");
    auto* menu = sidebar->findChild<adqt::widgets::AdNavigationMenu*>();
    require(menu != nullptr, "main navigation menu");
    QModelIndex aboutIndex;
    for (int row = 0; row < menu->model()->rowCount(); ++row) {
        const auto index = menu->model()->index(row, 0);
        if (index.data(adqt::widgets::AdNavigationMenu::StableIdRole).toString() ==
            QStringLiteral("/about")) {
            aboutIndex = index;
            break;
        }
    }
    require(aboutIndex.isValid() && aboutIndex.data(Qt::DecorationRole).isValid(),
            "About has a top-level navigation item and icon");
    menu->activated(aboutIndex);
    flushEvents();
    auto* page = window.findChild<AboutPageWidget*>();
    require(page != nullptr && page->isVisible() &&
                card->currentRoute() == QStringLiteral("/about") &&
                sidebar->currentRoute() == QStringLiteral("/about"),
            "sidebar activation opens the About page in the main interface");
    require(card->currentSections().isEmpty() &&
                !child<adqt::widgets::AdTabs>(*header, "mainSectionTabs")->isVisible(),
            "About hides settings section tabs");

    const settings::SettingsSearchIndex search(registry);
    const auto results = search.search(QStringLiteral("version"));
    require(!results.isEmpty() && results.constFirst().location.pageId == QStringLiteral("about"),
            "version search finds About");
    QPointer<AboutPageWidget> previous(page);
    card->setCurrentRoute(QStringLiteral("/settings/generalSettings"));
    flushEvents();
    require(previous.isNull(), "leaving About releases its page and connections");
    header->locationRequested(results.constFirst().location);
    flushEvents();
    page = window.findChild<AboutPageWidget*>();
    require(page != nullptr && sidebar->currentRoute() == QStringLiteral("/about"),
            "search navigation opens About and synchronizes the sidebar");

    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeManager::instance().setThemeAppearance(appearance);
        flushEvents();
        const auto scheme = styles::ThemeManager::instance().themeColorScheme();
        require(
            child<QLabel>(*page, "aboutVersionValue")->palette().color(QPalette::WindowText) ==
                    scheme.map.colorText &&
                child<QLabel>(*page, "aboutDescription")->palette().color(QPalette::WindowText) ==
                    scheme.map.colorTextSecondary,
            "About typography follows the active theme");
        require(child<QFrame>(*page, "aboutVersionPanel")
                    ->styleSheet()
                    .contains(scheme.map.colorPrimaryBg.name(QColor::HexArgb)),
                "version surface follows the primary theme token");
        require(!child<QLabel>(*page, "aboutLogo")->pixmap().isNull(), "render the Snow Shot icon");
        snapshot(window, appearance == styles::ThemeAppearance::Light
                             ? QStringLiteral("about-light")
                             : QStringLiteral("about-dark"));
    }

    styles::ThemeManager::instance().setThemeAppearance(styles::ThemeAppearance::Light);
    for (const QString& locale :
         {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR) +
                                QStringLiteral("/snow_shot_%1.qm").arg(locale)),
                "load a compiled application translation catalog");
        QCoreApplication::installTranslator(&translator);
        flushEvents();
        const QString translatedTitle = translator.translate("AboutPageWidget", "About Snow Shot");
        require(!translatedTitle.isEmpty() && page->accessibleName() == translatedTitle,
                "an open About page retranslates immediately");
        require(child<QLabel>(*page, "aboutVersionValue")->text() ==
                    QStringLiteral(SNOW_SHOT_TEST_VERSION),
                "language changes preserve the release version");
        require(child<adqt::widgets::AdButton>(*page, "aboutCopyVersion")->text() ==
                    translator.translate("AboutPageWidget", "Copy version"),
                "copy action retranslates");
        require(
            child<adqt::widgets::AdDescriptions>(*page, "aboutDetails")->itemAt(0).content ==
                translator.translate("AboutPageWidget", "GNU General Public License v3.0 or later"),
            "license details retranslate");
        require(sidebar->currentRoute() == QStringLiteral("/about"),
                "translated navigation preserves About selection");
        auto* scroll = page->findChild<adqt::widgets::AdScrollArea*>();
        require(scroll != nullptr && scroll->verticalScrollBar()->maximum() == 0,
                "all About information fits in the default window size");
        snapshot(window, QStringLiteral("about-%1").arg(locale));
        window.resize(512, 316);
        sidebar->setCollapsed(true);
        flushEvents();
        require(scroll != nullptr && scroll->horizontalScrollBar()->maximum() == 0,
                "narrow About page fits without horizontal scrolling");
        require(scroll->verticalScrollBar()->maximum() > 0,
                "short windows can scroll to all About content");
        snapshot(window, QStringLiteral("about-%1-compact").arg(locale));
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        flushEvents();
        snapshot(window, QStringLiteral("about-%1-compact-bottom").arg(locale));
        window.resize(900, 556);
        sidebar->setCollapsed(false);
        QCoreApplication::removeTranslator(&translator);
        flushEvents();
    }
    window.hide();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    // Static Qt's offscreen font database does not discover Windows fonts.
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    for (const QString& file : {QStringLiteral("segoeui.ttf"), QStringLiteral("seguisb.ttf"),
                                QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")}) {
        if (QFileInfo::exists(fonts.filePath(file))) {
            QFontDatabase::addApplicationFont(fonts.filePath(file));
        }
    }
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("about_page_tests"));
    QTemporaryDir directory;
    require(directory.isValid(), "isolated About test storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 8000}).success,
            "initialize isolated storage");
    styles::ThemeManager::instance().initialize(application);
    versionIsExactSelectableAndCopyable();
    absentVersionDoesNotInventARelease();
    mainNavigationSearchThemesAndLanguages();
    storage.shutdown();
    return 0;
}
