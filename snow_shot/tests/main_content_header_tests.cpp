#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/applicationsearchwidget.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/select.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QLayout>
#include <QListView>
#include <QPalette>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
const snow_shot::presentation::settings::SettingsCatalog& catalog() {
    return snow_shot::presentation::settings::builtInSettingsCatalog();
}

QVector<snow_shot::presentation::settings::SettingsSectionSummary> quickFunctionSections() {
    return catalog().sectionSummaries(QStringLiteral("quick-functions"));
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

void headerPlacesSearchAboveAntDesignTabs() {
    using snow_shot::presentation::styles::ThemeManager;

    MainContentHeaderWidget header(catalog(),
                                   ThemeManager::instance().themeColorScheme().metricAlias);
    header.setSections(quickFunctionSections());
    header.resize(680, header.sizeHint().height());
    header.show();
    flushEvents();

    auto* search = header.findChild<ApplicationSearchWidget*>(QStringLiteral("globalTopSearchBar"));
    auto* select = search != nullptr ? search->findChild<adqt::widgets::AdSelect*>() : nullptr;
    auto* tabs = header.findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));

    require(header.objectName() == QStringLiteral("mainContentHeader") &&
                header.autoFillBackground(),
            "the content header should be an independent themed surface");
    require(search != nullptr && select != nullptr,
            "the content header should expose the promoted global search");
    require(tabs != nullptr && tabs->type() == adqt::widgets::AdTabs::Type::Line,
            "the content header should use ant_design_qt line tabs");
    require(tabs->animated(),
            "the content header should keep ant_design_qt tab transitions enabled");
    require(search->geometry().bottom() <= tabs->geometry().top(),
            "the global search should sit above the page tabs");
    require(search->width() >= 280 && search->width() <= 400,
            "the global search should retain a stable readable width");
    require(tabs->count() == 2 &&
                tabs->tabKey(0) == QStringLiteral("screenshot") &&
                tabs->tabKey(1) == QStringLiteral("other"),
            "tabs should identify categories on the current quick-functions page");
    require(tabs->tabText(0) == QStringLiteral("Screenshot") &&
                tabs->tabText(1) == QStringLiteral("Other"),
            "tabs should use the page's major section labels");
    require(select->options().size() == 4,
            "global search should initially show only page entries");
    const auto searchOptions = select->options();
    require(select->itemDelegate() != nullptr && searchOptions.constFirst().group.isEmpty(),
            "search results should use the custom row renderer instead of group headers");
    const QString descriptionRole = QStringLiteral("__role_%1").arg(Qt::UserRole + 101);
    const QString categoryRole = QStringLiteral("__role_%1").arg(Qt::UserRole + 102);
    require(searchOptions.constFirst().metadata.value(descriptionRole).toString() ==
                QStringLiteral("Quick Functions page") &&
                searchOptions.constFirst().metadata.value(categoryRole).toString() ==
                    QStringLiteral("Pages"),
            "search rows should expose their description and right-aligned category context");
    for (const auto& option : searchOptions) {
        require(option.metadata.value(categoryRole).toString() == QStringLiteral("Pages"),
                "default search results should contain only the Pages category");
    }

    select->setSearchText(QStringLiteral("theme"));
    flushEvents();
    const auto filteredOptions = select->options();
    require(!filteredOptions.isEmpty() &&
                filteredOptions.constFirst().value.toString() ==
                    QStringLiteral("item:interface.theme") &&
                filteredOptions.constFirst().metadata.value(categoryRole).toString() ==
                    QStringLiteral("Interface Settings / General"),
            "typed searches should still include matching section and item entries");
    select->setSearchText(QString());
    flushEvents();
    require(select->options().size() == 4,
            "clearing the search should restore the page-only defaults");

    require(snow_shot::storage::ScreenshotSettings().setDelaySeconds(7),
            "the delayed screenshot setting should be writable");
    select->setSearchText(QStringLiteral("delay 7s"));
    flushEvents();
    const auto delayOptions = select->options();
    require(!delayOptions.isEmpty() &&
                delayOptions.constFirst().value.toString() ==
                    QStringLiteral("item:quick.screenshot-delay") &&
                delayOptions.constFirst().label == QStringLiteral("Delay 7s to Execute") &&
                !delayOptions.constFirst().label.contains(QStringLiteral("%1")),
            "global search should render the current delayed screenshot value without placeholders");
    require(snow_shot::storage::ScreenshotSettings().setDelaySeconds(4),
            "the delayed screenshot setting should support live updates");
    select->setSearchText(QStringLiteral("delay 4s"));
    flushEvents();
    const auto updatedDelayOptions = select->options();
    require(!updatedDelayOptions.isEmpty() &&
                updatedDelayOptions.constFirst().label == QStringLiteral("Delay 4s to Execute"),
            "global search should refresh delayed screenshot text when the setting changes");
    select->setSearchText(QString());
    flushEvents();
    select->showPopup();
    flushEvents();
    QListView* resultList = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* candidate = qobject_cast<QListView*>(widget);
        if (candidate != nullptr && candidate->isVisible() && candidate->model() != nullptr &&
            candidate->model()->rowCount() == 4) {
            resultList = candidate;
            break;
        }
    }
    require(resultList != nullptr && resultList->model() != nullptr,
            "the search popup should expose its result list");
    require(resultList->model()->rowCount() == 4,
            "the default search popup should render one row per page without group headers");
    require(resultList->sizeHintForRow(0) >= 52,
            "search result rows should be tall enough for title and description text");
    require(resultList->width() == select->width(),
            "the search result popup should match the global search control width");
    QWidget* resultPopup = resultList->window();
    if (resultPopup == &header) {
        QWidget* candidate = resultList;
        while (candidate != nullptr &&
               candidate->objectName() != QStringLiteral("adselect-popup")) {
            candidate = candidate->parentWidget();
        }
        resultPopup = candidate;
    }
    require(resultPopup != nullptr,
            "the search result list should belong to an Ant Design popup surface");
    const int selectCenter = select->mapTo(&header, select->rect().center()).x();
    const int popupCenter =
        resultPopup->mapTo(&header, resultPopup->rect().center()).x();
    require(std::abs(selectCenter - popupCenter) <= 1,
            "the search result popup should be horizontally centered under the search control");
    const QString resultsSnapshotPath =
        qEnvironmentVariable("SNOW_SHOT_SEARCH_RESULTS_SNAPSHOT");
    if (!resultsSnapshotPath.isEmpty()) {
        const QImage snapshot = resultList->grab().toImage();
        require(!snapshot.isNull() && snapshot.save(resultsSnapshotPath),
                "the search results snapshot should be writable");
    }
    select->hidePopup();
    snow_shot::presentation::settings::SettingsLocation activatedLocation;
    QObject::connect(search, &ApplicationSearchWidget::locationActivated, &header,
                     [&activatedLocation](const auto& location) {
                         activatedLocation = location;
                     });
    select->selected(QStringLiteral("page:storage-and-privacy"),
                     QStringLiteral("Storage and Privacy"));
    require(activatedLocation.pageId == QStringLiteral("storage-and-privacy") &&
                activatedLocation.sectionId.isEmpty() && activatedLocation.itemId.isEmpty(),
            "global search should activate a structured storage page location");

    const QString snapshotPath = qEnvironmentVariable("SNOW_SHOT_MAIN_HEADER_SNAPSHOT");
    if (!snapshotPath.isEmpty()) {
        const QImage snapshot = header.grab().toImage();
        require(!snapshot.isNull() && snapshot.save(snapshotPath),
                "the main content header snapshot should be writable");
    }

    header.resize(292, header.sizeHint().height());
    flushEvents();
    require(search->geometry().left() >= 0 && search->geometry().right() < header.width(),
            "the global search should remain inside the header at minimum content width");
    require(search->geometry().bottom() <= tabs->geometry().top(),
            "search and tabs should not overlap at minimum content width");

    const QString narrowSnapshotPath =
        qEnvironmentVariable("SNOW_SHOT_MAIN_HEADER_NARROW_SNAPSHOT");
    if (!narrowSnapshotPath.isEmpty()) {
        const QImage snapshot = header.grab().toImage();
        require(!snapshot.isNull() && snapshot.save(narrowSnapshotPath),
                "the narrow main content header snapshot should be writable");
    }
}

void tabsRequestCategoriesWithoutChangingPages() {
    using snow_shot::presentation::styles::ThemeManager;

    MainContentHeaderWidget header(catalog(),
                                   ThemeManager::instance().themeColorScheme().metricAlias);
    header.setSections(quickFunctionSections());
    auto* tabs = header.findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));
    require(tabs != nullptr, "section tabs should exist");

    QStringList categoryRequests;
    QObject::connect(&header, &MainContentHeaderWidget::sectionRequested, &header,
                     [&categoryRequests](const QString& sectionId) {
                         categoryRequests.push_back(sectionId);
                     });

    tabs->setCurrentKey(QStringLiteral("other"));
    require(categoryRequests.isEmpty(),
            "programmatic tab synchronization should not request navigation");
    tabs->tabClicked(QStringLiteral("other"));
    require(categoryRequests == QStringList{QStringLiteral("other")},
            "clicking a tab should request its in-page category anchor");

    categoryRequests.clear();
    header.setCurrentSection(QStringLiteral("screenshot"));
    require(header.currentSection() == QStringLiteral("screenshot") &&
                categoryRequests.isEmpty(),
            "external category synchronization should not emit a navigation loop");

    header.setCurrentSection(QStringLiteral("section.unknown"));
    require(header.currentSection() == QStringLiteral("screenshot"),
            "unknown categories should resolve to the first current-page category");

    header.setSections(catalog().sectionSummaries(QStringLiteral("interface-settings")));
    require(tabs->count() == 1 &&
                tabs->tabKey(0) == QStringLiteral("general") &&
                tabs->tabText(0) == QStringLiteral("General"),
            "settings controls should remain within one major Interface Settings section");
    require(header.currentSection() == QStringLiteral("general") &&
                categoryRequests.isEmpty(),
            "rebuilding categories should select the first anchor without emitting a request");

    header.setSections(catalog().sectionSummaries(QStringLiteral("screenshot-history")));
    require(tabs->count() == 0 && tabs->isHidden() && header.layout() != nullptr &&
                header.layout()->contentsMargins().bottom() ==
                    header.layout()->contentsMargins().top(),
            "pages without sections should hide the tabs and preserve balanced search spacing");

    header.setSections(quickFunctionSections());
    require(tabs->count() == 2 && !tabs->isHidden() &&
                header.layout()->contentsMargins().bottom() == 0,
            "section tabs should become visible again with their original header spacing");
}

void headerSurfaceFollowsTheme() {
    using snow_shot::presentation::styles::ThemeAppearance;
    using snow_shot::presentation::styles::ThemeManager;

    auto& themeManager = ThemeManager::instance();
    MainContentHeaderWidget header(catalog(), themeManager.themeColorScheme().metricAlias);

    themeManager.setThemeAppearance(ThemeAppearance::Dark);
    flushEvents();
    require(header.palette().color(QPalette::Window) ==
                themeManager.themeColorScheme().map.colorBgContainer,
            "the content header should follow the dark container surface");

    themeManager.setThemeAppearance(ThemeAppearance::Light);
    flushEvents();
    require(header.palette().color(QPalette::Window) ==
                themeManager.themeColorScheme().map.colorBgContainer,
            "the content header should restore the light container surface");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("main_content_header_tests"));

    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(snow_shot::storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    headerPlacesSearchAboveAntDesignTabs();
    tabsRequestCategoriesWithoutChangingPages();
    headerSurfaceFollowsTheme();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
