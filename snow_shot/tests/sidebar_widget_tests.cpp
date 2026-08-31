#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/button.h"
#include "widgets/navigation_menu.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QPalette>
#include <QSize>
#include <QSizePolicy>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
constexpr int SIDEBAR_EXPANDED_WIDTH = 220;
constexpr int FIRST_TOP_LEVEL_MENU_TOP_SPACING = 8;
constexpr int COLLAPSE_TRIGGER_HEIGHT = 48;
constexpr int COLLAPSE_TRIGGER_ICON_SIZE = 18;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

QModelIndex findByStableId(const QAbstractItemModel* model, const QString& stableId,
                           const QModelIndex& parent = {}) {
    if (model == nullptr) {
        return {};
    }
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex candidate = model->index(row, 0, parent);
        if (candidate.data(adqt::widgets::AdNavigationMenu::StableIdRole).toString() == stableId) {
            return candidate;
        }
        const QModelIndex descendant = findByStableId(model, stableId, candidate);
        if (descendant.isValid()) {
            return descendant;
        }
    }
    return {};
}

int settingsNavigationPageCount() {
    const auto& navigation =
        snow_shot::presentation::settings::builtInSettingsRegistry().navigation();
    for (const auto& node : navigation) {
        const auto* group =
            std::get_if<snow_shot::presentation::settings::SettingsNavigationGroupDefinition>(
                &node);
        if (group != nullptr && group->id == QStringLiteral("nav.settings")) {
            return group->pages.size();
        }
    }
    return 0;
}

void navigationUsesAntDesignDefaultsAndCollapseTriggerStyle() {
    SidebarWidget sidebar(snow_shot::presentation::settings::builtInSettingsRegistry());
    flushEvents();

    auto* menu = sidebar.findChild<adqt::widgets::AdNavigationMenu*>();
    require(menu != nullptr, "sidebar should expose its navigation menu");
    require(menu->colorScheme() == adqt::widgets::AdNavigationMenu::ColorScheme::Inherit,
            "sidebar should inherit ant_design_qt navigation colors");
    require(menu->submenuTrigger() == adqt::widgets::AdNavigationMenu::TriggerSubMenuAction::Hover,
            "sidebar should retain ant_design_qt hover submenu behavior");
    const QModelIndex history = findByStableId(menu->model(), QStringLiteral("/history"));
    const QModelIndex settings = findByStableId(menu->model(), QStringLiteral("nav.settings"));
    const QModelIndex storageAndPrivacy =
        findByStableId(menu->model(), QStringLiteral("/settings/storageAndPrivacy"));
    require(history.isValid() &&
                history.data(Qt::DecorationRole).isValid() &&
                history.data(adqt::widgets::AdNavigationMenu::StableIdRole).toString() ==
                    QStringLiteral("/history") &&
                settings.isValid() && settings.data(Qt::DecorationRole).isValid() &&
                menu->model()->rowCount(settings) == settingsNavigationPageCount() &&
                menu->isExpanded(settings) &&
                !storageAndPrivacy.data(Qt::DecorationRole).isValid() &&
                storageAndPrivacy.data(adqt::widgets::AdNavigationMenu::StableIdRole).toString() ==
                    QStringLiteral("/settings/storageAndPrivacy"),
            "top-level items should keep icons while expanded submenu items omit them");
    sidebar.setCurrentRoute(QStringLiteral("/settings/storageAndPrivacy"));
    require(sidebar.currentRoute() == QStringLiteral("/settings/storageAndPrivacy"),
            "storage and privacy route should be selectable");

    const auto menuTokens = menu->componentTokens();
    require(!menuTokens.metrics.itemHeight.has_value() &&
                !menuTokens.metrics.itemPaddingInline.has_value() &&
                !menuTokens.metrics.indentation.has_value() &&
                menuTokens.metrics.rootPaddingBlockStart ==
                    FIRST_TOP_LEVEL_MENU_TOP_SPACING &&
                !menuTokens.colors.shared.itemBackground.has_value() &&
                !menuTokens.colors.shared.itemSelectedBackground.has_value(),
            "sidebar should only override the root content top padding token");

    const auto menuStyles = menu->semanticStyles();
    require(!menuStyles.root.backgroundColor.has_value() &&
                !menuStyles.item.backgroundColor.has_value() &&
                !menuStyles.popup.backgroundColor.has_value(),
            "sidebar should not override ant_design_qt navigation semantic styles");

    auto* button =
        sidebar.findChild<adqt::widgets::AdButton*>(QStringLiteral("sidebarCollapseButton"));
    require(button != nullptr, "sidebar should expose a bottom collapse button");
    require(button->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Text &&
                button->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral &&
                button->shape() == adqt::widgets::AdButton::Shape::Rectangle &&
                button->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding &&
                button->sizePolicy().verticalPolicy() == QSizePolicy::Fixed &&
                button->height() == COLLAPSE_TRIGGER_HEIGHT &&
                button->iconSize() == QSize(COLLAPSE_TRIGGER_ICON_SIZE, COLLAPSE_TRIGGER_ICON_SIZE),
            "sidebar collapse button should use the configured trigger style");
}

void firstTopLevelMenuAndCollapseTriggerUseThemeBackground() {
    snow_shot::storage::InterfaceSettings settings;
    settings.setSidebarCollapsed(false);

    using snow_shot::presentation::styles::ThemeAppearance;
    using snow_shot::presentation::styles::ThemeManager;
    auto& themeManager = ThemeManager::instance();
    themeManager.setThemeAppearance(ThemeAppearance::Light);

    SidebarWidget sidebar(snow_shot::presentation::settings::builtInSettingsRegistry());
    sidebar.resize(256, 480);
    sidebar.show();
    flushEvents();

    require(sidebar.autoFillBackground() &&
                sidebar.palette().color(QPalette::Window) ==
                    themeManager.themeColorScheme().map.colorBgContainer,
            "sidebar background should use the configured container color");

    auto* menu = sidebar.findChild<adqt::widgets::AdNavigationMenu*>();
    require(menu != nullptr, "sidebar should expose its navigation menu");
    require(menu->geometry().top() == 0, "sidebar navigation should start at the top edge");
    require(menu->contentsMargins().top() == 0,
            "sidebar root geometry should not be inset to space its menu items");
    require(menu->autoFillBackground() && menu->palette().color(QPalette::Window) ==
                                              themeManager.themeColorScheme().map.colorBgContainer,
            "sidebar navigation background should use the configured surface color");

    auto* inlineView = menu->findChild<QTreeView*>(QStringLiteral("AdNavigationMenu-inline-view"));
    require(inlineView != nullptr, "sidebar should expose its inline navigation view");
    require(inlineView->geometry().top() == 0,
            "the inline view and its root border should begin at the navigation top edge");
    const QModelIndex firstItemIndex = inlineView->model()->index(0, 0, inlineView->rootIndex());
    const QRect firstItemRect = inlineView->visualRect(firstItemIndex);
    require(firstItemIndex.isValid() && firstItemRect.isValid(),
            "quick functions should be visible");
    require(inlineView->viewport()->mapTo(menu, firstItemRect.topLeft()).y() ==
                FIRST_TOP_LEVEL_MENU_TOP_SPACING,
            "the first top-level item should begin after the configured top spacing");

    auto* trigger = sidebar.findChild<QFrame*>(QStringLiteral("sidebarCollapseTrigger"));
    require(trigger != nullptr, "sidebar should expose a collapse trigger background");
    require(trigger->autoFillBackground() &&
                trigger->palette().color(QPalette::Window) ==
                    themeManager.themeColorScheme().map.colorBgContainer &&
                trigger->height() == COLLAPSE_TRIGGER_HEIGHT &&
                trigger->geometry().bottom() == sidebar.contentsRect().bottom(),
            "sidebar collapse trigger should use the light theme container color and be flush with "
            "the bottom edge");

    themeManager.setThemeAppearance(ThemeAppearance::Dark);
    flushEvents();
    const QColor darkBackground = themeManager.themeColorScheme().map.colorBgContainer;
    require(sidebar.palette().color(QPalette::Window) == darkBackground &&
                menu->palette().color(QPalette::Window) == darkBackground &&
                trigger->autoFillBackground() &&
                trigger->palette().color(QPalette::Window) == QColor(QStringLiteral("#00203F")),
            "sidebar backgrounds should update when the theme changes");

    sidebar.hide();
    themeManager.setThemeAppearance(ThemeAppearance::Light);
    settings.setSidebarCollapsed(false);
}

void collapseButtonSwitchesNavigationMode() {
    snow_shot::storage::InterfaceSettings settings;
    settings.setSidebarCollapsed(false);

    SidebarWidget sidebar(snow_shot::presentation::settings::builtInSettingsRegistry());
    flushEvents();

    require(!sidebar.isCollapsed(), "sidebar should be expanded by default");
    const int expandedWidth = sidebar.width();
    require(expandedWidth == SIDEBAR_EXPANDED_WIDTH,
            "expanded sidebar should use the configured width");

    auto* button =
        sidebar.findChild<adqt::widgets::AdButton*>(QStringLiteral("sidebarCollapseButton"));
    require(button != nullptr, "sidebar should expose a bottom collapse button");

    auto* menu = sidebar.findChild<adqt::widgets::AdNavigationMenu*>();
    require(menu != nullptr, "sidebar should expose its navigation menu");
    require(!menu->collapsed(), "navigation menu should begin expanded");

    button->click();
    flushEvents();

    require(sidebar.isCollapsed(), "collapse button should collapse the sidebar");
    require(menu->collapsed(), "collapse button should collapse the navigation menu");
    require(sidebar.width() < expandedWidth, "collapsed sidebar should be narrower");
    require(settings.sidebarCollapsed(),
            "collapsed sidebar state should be persisted");

    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&sidebar, &languageChange);
    flushEvents();

    button->click();
    flushEvents();

    require(!sidebar.isCollapsed(), "collapse button should expand the sidebar on a second click");
    require(!menu->collapsed(), "navigation menu should expand on a second click");
    require(menu->isExpanded(findByStableId(menu->model(), QStringLiteral("nav.settings"))),
            "submenus should remain expanded after a collapsed navigation model rebuild");
    require(sidebar.width() == expandedWidth, "expanded sidebar should restore its width");

    settings.setSidebarCollapsed(false);
}

void collapsedSubmenuUsesNaturalPopupHeight() {
    snow_shot::storage::InterfaceSettings settings;
    settings.setSidebarCollapsed(false);

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* sidebar = new SidebarWidget(
        snow_shot::presentation::settings::builtInSettingsRegistry(), &window);
    layout->addWidget(sidebar);
    window.resize(640, 480);
    window.show();
    flushEvents();

    auto* menu = sidebar->findChild<adqt::widgets::AdNavigationMenu*>();
    require(menu != nullptr, "sidebar should expose its navigation menu");
    const QModelIndex settingsIndex =
        findByStableId(menu->model(), QStringLiteral("nav.settings"));
    require(settingsIndex.isValid(), "settings should be represented by a menu node");

    sidebar->setCollapsed(true);
    menu->setCurrentIndex(settingsIndex);
    menu->setExpanded(settingsIndex, true);
    flushEvents();

    auto* popup = window.findChild<QWidget*>(QStringLiteral("AdNavigationMenu-popup-window"));
    require(popup != nullptr && popup->isVisible(), "collapsed submenu should open its popup");

    auto* popupView = popup->findChild<QTreeView*>();
    require(popupView != nullptr, "submenu popup should contain the navigation tree view");
    const int submenuRows = popupView->model()->rowCount(popupView->rootIndex());
    const QModelIndex firstChild = popupView->model()->index(0, 0, popupView->rootIndex());
    const QModelIndex lastChild =
        popupView->model()->index(submenuRows - 1, 0, popupView->rootIndex());
    const QRect firstChildRect = popupView->visualRect(firstChild);
    const QRect lastChildRect = popupView->visualRect(lastChild);
    require(submenuRows == settingsNavigationPageCount() && firstChildRect.isValid() &&
                lastChildRect.isValid() && firstChildRect.bottom() < lastChildRect.top(),
            "the popup should render every registry settings page on a distinct row");

    const int bottomInset = popupView->viewport()->height() - lastChildRect.bottom() - 1;
    require(bottomInset <= firstChildRect.top() + 2,
            "settings submenu popup should not reserve an empty row below its content");

    window.hide();
    settings.setSidebarCollapsed(false);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("sidebar_widget_tests"));
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(snow_shot::storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));

    navigationUsesAntDesignDefaultsAndCollapseTriggerStyle();
    firstTopLevelMenuAndCollapseTriggerUseThemeBackground();
    collapseButtonSwitchesNavigationMode();
    collapsedSubmenuUsesNaturalPopupHeight();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
