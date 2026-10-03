#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/applicationsearchwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "widgets/button.h"
#include "widgets/scroll_area.h"
#include <QHBoxLayout>
#include <QTranslator>
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/select.h"
#include "widgets/detail/popup_shadow.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QFontMetrics>
#include <QLayout>
#include <QScrollBar>
#include <QListView>
#include <QPalette>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QAbstractItemDelegate>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
const snow_shot::presentation::settings::SettingsRegistry& registry() {
    return snow_shot::presentation::settings::builtInSettingsRegistry();
}

const snow_shot::presentation::settings::SettingsCatalog& catalog() {
    return registry().catalog();
}

QVector<snow_shot::presentation::settings::SettingsSectionSummary> globalHotkeySections() {
    return catalog().sectionSummaries(QStringLiteral("global-hotkeys"));
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

void sidebarPlacesSearchAboveNavigation() {
    using snow_shot::presentation::styles::ThemeManager;

    QWidget host;
    MainContentHeaderWidget header(ThemeManager::instance().themeColorScheme().metricAlias);
    auto* layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    SidebarWidget sidebar(registry());
    sidebar.setCollapsed(false);
    layout->addWidget(&sidebar);
    layout->addWidget(&header, 1, Qt::AlignTop);
    header.setSections(globalHotkeySections());
    host.resize(900, 540);
    host.show();
    flushEvents();

    auto* search =
        sidebar.findChild<ApplicationSearchWidget*>(QStringLiteral("globalTopSearchBar"));
    auto* select = search != nullptr ? search->findChild<adqt::widgets::AdSelect*>() : nullptr;
    auto* tabs = header.findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));

    require(header.objectName() == QStringLiteral("mainContentHeader") &&
                header.autoFillBackground(),
            "the content header should be an independent themed surface");
    require(search != nullptr && select != nullptr,
            "the sidebar should expose global search above its navigation");
    require(tabs != nullptr && tabs->type() == adqt::widgets::AdTabs::Type::Line,
            "the content header should use ant_design_qt line tabs");
    require(tabs->animated(),
            "the content header should keep ant_design_qt tab transitions enabled");
    auto* scroll = sidebar.findChild<adqt::widgets::AdScrollArea*>();
    require(scroll != nullptr &&
                search->mapTo(&sidebar, search->rect().bottomLeft()).y() < scroll->geometry().top(),
            "search should sit above the independently scrollable navigation");
    const auto& metric = ThemeManager::instance().themeColorScheme().metricAlias;
    require(search->width() == sidebar.width() - 2 * metric.paddingSM &&
                search->mapTo(&sidebar, QPoint()).x() == metric.paddingSM &&
                search->mapTo(&sidebar, QPoint()).y() == metric.paddingXS,
            "sidebar search should have compact top spacing and balanced side spacing");
    require(select->placeholder() == QStringLiteral("Search Function") &&
                select->accessibleName() == QStringLiteral("Search Function"),
            "the search placeholder and accessible name should use the requested copy");
    require(header.findChild<ApplicationSearchWidget*>() == nullptr,
            "the page header should only contain page tabs");
    const auto globalHotkeySectionsForPage = globalHotkeySections();
    require(tabs->count() == globalHotkeySectionsForPage.size(),
            "tabs should cover every category on the current global-hotkeys page");
    for (int index = 0; index < globalHotkeySectionsForPage.size(); ++index) {
        require(tabs->tabKey(index) == globalHotkeySectionsForPage.at(index).id &&
                    tabs->tabText(index) == globalHotkeySectionsForPage.at(index).label,
                "tabs should preserve registry section IDs, order, and labels");
    }
    const qsizetype pageCount = registry().pages().size();
    require(select->options().size() == pageCount,
            "global search should initially show only page entries");
    const auto searchOptions = select->options();
    require(select->itemDelegate() != nullptr && searchOptions.constFirst().group.isEmpty(),
            "search results should use the custom row renderer instead of group headers");
    const QString descriptionRole = QStringLiteral("__role_%1").arg(Qt::UserRole + 101);
    const QString categoryRole = QStringLiteral("__role_%1").arg(Qt::UserRole + 102);
    require(searchOptions.constFirst().metadata.value(descriptionRole).toString() ==
                    QStringLiteral("Global hotkeys page") &&
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
                    QStringLiteral("Appearance / Theme configuration"),
            "typed searches should still include matching section and item entries");
    select->setSearchText(QString());
    flushEvents();
    require(select->options().size() == pageCount,
            "clearing the search should restore the page-only defaults");

    require(snow_shot::storage::ScreenshotSettings().setDelaySeconds(7),
            "the delayed screenshot setting should be writable");
    select->setSearchText(QStringLiteral("delay 7s"));
    flushEvents();
    const auto delayOptions = select->options();
    require(
        !delayOptions.isEmpty() &&
            delayOptions.constFirst().value.toString() ==
                QStringLiteral("item:quick.screenshot-delay") &&
            delayOptions.constFirst().label == QStringLiteral("Delay 7s to execute") &&
            !delayOptions.constFirst().label.contains(QStringLiteral("%1")),
        "global search should render the current delayed screenshot value without placeholders");
    require(snow_shot::storage::ScreenshotSettings().setDelaySeconds(4),
            "the delayed screenshot setting should support live updates");
    select->setSearchText(QStringLiteral("delay 4s"));
    flushEvents();
    const auto updatedDelayOptions = select->options();
    require(!updatedDelayOptions.isEmpty() &&
                updatedDelayOptions.constFirst().label == QStringLiteral("Delay 4s to execute"),
            "global search should refresh delayed screenshot text when the setting changes");
    select->setSearchText(QString());
    flushEvents();
    select->showPopup();
    flushEvents();
    QListView* resultList = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* candidate = qobject_cast<QListView*>(widget);
        if (candidate != nullptr && candidate->isVisible() && candidate->model() != nullptr &&
            candidate->model()->rowCount() == pageCount) {
            resultList = candidate;
            break;
        }
    }
    require(resultList != nullptr && resultList->model() != nullptr,
            "the search popup should expose its result list");
    require(resultList->model()->rowCount() == pageCount,
            "the default search popup should render one row per page without group headers");
    require(resultList->sizeHintForRow(0) >= 52,
            "search result rows should be tall enough for title and description text");
    require(!select->popupMatchSelectWidth() && resultList->width() > select->width(),
            "sidebar search results should retain a readable width beyond the narrow field");
    QWidget* resultPopup = resultList->window();
    if (resultPopup == &host) {
        QWidget* candidate = resultList;
        while (candidate != nullptr &&
               candidate->objectName() != QStringLiteral("adselect-popup")) {
            candidate = candidate->parentWidget();
        }
        resultPopup = candidate;
    }
    require(resultPopup != nullptr,
            "the search result list should belong to an Ant Design popup surface");
    const int selectLeft = select->mapToGlobal(QPoint()).x();
    const int popupLeft =
        resultPopup
            ->mapToGlobal(adqt::widgets::detail::antPopupShadowVisualRect(resultPopup->rect())
                              .topLeft()
                              .toPoint())
            .x();
    require(std::abs(selectLeft - popupLeft) <= 1,
            "search results should align with the left edge of the sidebar field");
    const QString resultsSnapshotPath = qEnvironmentVariable("SNOW_SHOT_SEARCH_RESULTS_SNAPSHOT");
    if (!resultsSnapshotPath.isEmpty()) {
        const QImage snapshot = resultList->grab().toImage();
        require(!snapshot.isNull() && snapshot.save(resultsSnapshotPath),
                "the search results snapshot should be writable");
    }
    select->hidePopup();
    snow_shot::presentation::settings::SettingsLocation activatedLocation;
    QObject::connect(&sidebar, &SidebarWidget::locationRequested, &host,
                     [&activatedLocation](const auto& location) { activatedLocation = location; });
    select->selected(QStringLiteral("page:files-history"), QStringLiteral("Export & storage"));
    require(activatedLocation.pageId == QStringLiteral("files-history") &&
                activatedLocation.sectionId.isEmpty() && activatedLocation.itemId.isEmpty(),
            "global search should activate a structured storage page location");

    const QString snapshotPath = qEnvironmentVariable("SNOW_SHOT_MAIN_HEADER_SNAPSHOT");
    if (!snapshotPath.isEmpty()) {
        const QImage snapshot = host.grab().toImage();
        require(!snapshot.isNull() && snapshot.save(snapshotPath),
                "the main content header snapshot should be writable");
    }

    host.resize(512, 316);
    flushEvents();
    require(search->mapTo(&sidebar, search->rect().topLeft()).x() >= 0 &&
                search->mapTo(&sidebar, search->rect().topRight()).x() < sidebar.width(),
            "the search should remain inside the sidebar at minimum window size");
    require(header.geometry().right() < host.width(),
            "tabs should fit the remaining content width at minimum window size");
    const auto verifyFixedSearchGap = [&](QWidget* searchControl) {
        const QPoint searchPosition = searchControl->mapTo(&sidebar, QPoint());
        require(scroll->verticalScrollBar()->maximum() > 0,
                "the spacing regression should exercise overflowing navigation");
        for (const int position : {0, scroll->verticalScrollBar()->maximum()}) {
            scroll->verticalScrollBar()->setValue(position);
            flushEvents();
            const int searchBottom =
                searchControl->mapTo(&sidebar, searchControl->rect().bottomLeft()).y() + 1;
            require(scroll->mapTo(&sidebar, QPoint()).y() - searchBottom == metric.paddingXXS &&
                        searchControl->mapTo(&sidebar, QPoint()) == searchPosition,
                    "search must own a fixed gap above the menu at every scroll position");
        }
        scroll->verticalScrollBar()->setValue(0);
    };
    verifyFixedSearchGap(search);
    sidebar.setCollapsed(true);
    flushEvents();
    auto* searchButton =
        sidebar.findChild<adqt::widgets::AdButton*>(QStringLiteral("sidebarSearchButton"));
    require(searchButton != nullptr && searchButton->isVisible() && search->isHidden() &&
                sidebar.width() == 80,
            "collapsed navigation should offer an accessible search icon without clipping text");
    verifyFixedSearchGap(searchButton);
    searchButton->click();
    flushEvents();
    require(!sidebar.isCollapsed() && search->isVisible() && searchButton->isHidden() &&
                (select->hasFocus() || select->isAncestorOf(QApplication::focusWidget())),
            "the collapsed search button should expand navigation and focus search");
    const QString narrowSnapshotPath =
        qEnvironmentVariable("SNOW_SHOT_MAIN_HEADER_NARROW_SNAPSHOT");
    if (!narrowSnapshotPath.isEmpty()) {
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        flushEvents();
        const QImage snapshot = host.grab().toImage();
        require(!snapshot.isNull() && snapshot.save(narrowSnapshotPath),
                "the narrow main content header snapshot should be writable");
    }
}

void tabsRequestCategoriesWithoutChangingPages() {
    using snow_shot::presentation::styles::ThemeManager;

    MainContentHeaderWidget header(ThemeManager::instance().themeColorScheme().metricAlias);
    require(header.isHidden(), "a new header should not reserve space before it has tabs");
    header.setSections(globalHotkeySections());
    auto* tabs = header.findChild<adqt::widgets::AdTabs*>(QStringLiteral("mainSectionTabs"));
    require(tabs != nullptr, "section tabs should exist");

    QStringList categoryRequests;
    QObject::connect(
        &header, &MainContentHeaderWidget::sectionRequested, &header,
        [&categoryRequests](const QString& sectionId) { categoryRequests.push_back(sectionId); });

    tabs->setCurrentKey(QStringLiteral("other"));
    require(categoryRequests.isEmpty(),
            "programmatic tab synchronization should not request navigation");
    tabs->tabClicked(QStringLiteral("other"));
    require(categoryRequests == QStringList{QStringLiteral("other")},
            "clicking a tab should request its in-page category anchor");

    categoryRequests.clear();
    header.setCurrentSection(QStringLiteral("screenshot"));
    require(header.currentSection() == QStringLiteral("screenshot") && categoryRequests.isEmpty(),
            "external category synchronization should not emit a navigation loop");

    header.setCurrentSection(QStringLiteral("section.unknown"));
    require(header.currentSection() == QStringLiteral("screenshot"),
            "unknown categories should resolve to the first current-page category");

    const auto interfaceSections = catalog().sectionSummaries(QStringLiteral("general-appearance"));
    header.setSections(interfaceSections);
    require(tabs->count() == interfaceSections.size(),
            "Interface settings tabs should cover every registry section");
    for (int index = 0; index < interfaceSections.size(); ++index) {
        require(tabs->tabKey(index) == interfaceSections.at(index).id &&
                    tabs->tabText(index) == interfaceSections.at(index).label,
                "Interface settings tabs should preserve registry IDs, order, and labels");
    }
    require(!interfaceSections.isEmpty() &&
                header.currentSection() == interfaceSections.constFirst().id &&
                categoryRequests.isEmpty(),
            "rebuilding categories should select the first anchor without emitting a request");

    header.setSections(catalog().sectionSummaries(QStringLiteral("screenshot-history")));
    require(tabs->count() == 0 && tabs->isHidden() && header.isHidden(),
            "pages without sections should hide the entire top component");

    header.setSections(globalHotkeySections());
    require(tabs->count() == globalHotkeySections().size() && !tabs->isHidden() &&
                !header.isHidden() && header.layout()->contentsMargins().bottom() == 0,
            "section tabs should become visible again with their original header spacing");
}

void searchRetranslatesWithNavigation() {
    class SearchTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (qstrcmp(context, "SidebarWidget") == 0 && qstrcmp(source, "Search Function") == 0) {
                return QStringLiteral("Translated search");
            }
            return {};
        }
    } translator;
    SidebarWidget sidebar(registry());
    auto* select = sidebar.findChild<adqt::widgets::AdSelect*>();
    auto* button =
        sidebar.findChild<adqt::widgets::AdButton*>(QStringLiteral("sidebarSearchButton"));
    QCoreApplication::installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&sidebar, &languageChange);
    require(select->placeholder() == QStringLiteral("Translated search") &&
                button->accessibleName() == select->placeholder() &&
                button->toolTip() == select->placeholder(),
            "search copy should retranslate in both expanded and collapsed navigation");
    QCoreApplication::removeTranslator(&translator);
}

void headerSurfaceFollowsTheme() {
    using snow_shot::presentation::styles::ThemeAppearance;
    using snow_shot::presentation::styles::ThemeManager;

    auto& themeManager = ThemeManager::instance();
    MainContentHeaderWidget header(themeManager.themeColorScheme().metricAlias);

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

void searchTagsLeaveDescriptionsFullWidth() {
    using snow_shot::presentation::styles::ThemeAppearance;
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    SidebarWidget sidebar(registry());
    auto* search =
        sidebar.findChild<ApplicationSearchWidget*>(QStringLiteral("globalTopSearchBar"));
    auto* select = search->findChild<adqt::widgets::AdSelect*>();
    QStandardItemModel model(1, 1);
    const QModelIndex index = model.index(0, 0);
    model.setData(index, QStringLiteral("Search result title"),
                  adqt::widgets::AdSelect::DefaultLabelRole);
    model.setData(index,
                  QStringLiteral("A long description that uses the entire available row width"),
                  Qt::UserRole + 101);
    for (const auto appearance : {ThemeAppearance::Light, ThemeAppearance::Dark}) {
        themeManager.setThemeAppearance(appearance);
        flushEvents();
        for (const int width : {292, 400}) {
            QStyleOptionViewItem option;
            option.initFrom(select);
            option.font = select->font();
            option.rect = QRect(0, 0, width, 56);
            option.rect.setHeight(select->itemDelegate()->sizeHint(option, index).height());
            const auto render = [&]() {
                QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                select->itemDelegate()->paint(&painter, option, index);
                return image;
            };
            model.setData(index, QString(), Qt::UserRole + 102);
            const QImage withoutTag = render();
            model.setData(index, QStringLiteral("Interface settings / General"),
                          Qt::UserRole + 102);
            QImage previous;
            for (int kind = 0; kind < 3; ++kind) {
                model.setData(index, kind, Qt::UserRole + 103);
                const QImage withTag = render();
                QFont descriptionFont = option.font;
                descriptionFont.setPixelSize(
                    themeManager.themeColorScheme().metricAlias.fontSizeSM);
                const int descriptionHeight = QFontMetrics(descriptionFont).height();
                const int bottomPadding = 2 + themeManager.themeColorScheme().metricAlias.paddingXS;
                const QRect descriptionRect(
                    0, option.rect.height() - bottomPadding - descriptionHeight, width,
                    descriptionHeight);
                require(withTag.copy(descriptionRect) == withoutTag.copy(descriptionRect),
                        "category tags should not reduce or overlap the description line");
                require(withTag != withoutTag && (previous.isNull() || withTag != previous),
                        "page, section, and item tags should have distinct themed colors");
                previous = withTag;
            }
        }
    }
    themeManager.setThemeAppearance(ThemeAppearance::Light);
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
    // The translation page is hidden by default; the search expectations below
    // count every registered settings page, so surface it explicitly.
    require(snow_shot::storage::ExtendedFeaturesSettings().setTranslationPageEnabled(true),
            "translation page should be enabled for the search expectations");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    sidebarPlacesSearchAboveNavigation();
    tabsRequestCategoriesWithoutChangingPages();
    searchRetranslatesWithNavigation();
    headerSurfaceFollowsTheme();
    searchTagsLeaveDescriptionsFullWidth();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
