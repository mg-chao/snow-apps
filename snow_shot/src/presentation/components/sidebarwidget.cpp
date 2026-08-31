#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QEvent>
#include <QItemSelectionModel>
#include <QPalette>
#include <QSize>
#include <QSizePolicy>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVariant>
#include <QVBoxLayout>

#include "antd_icons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "widgets/button.h"
#include "widgets/navigation_menu.h"

namespace {
using adqt::widgets::AdNavigationMenu;
namespace outlined_icons = adqt::icons::antd::outlined;

constexpr int SIDEBAR_EXPANDED_WIDTH = 220;
constexpr int SIDEBAR_COLLAPSED_WIDTH = 80;
constexpr int FIRST_TOP_LEVEL_MENU_TOP_SPACING = 8;
constexpr int COLLAPSE_TRIGGER_HEIGHT = 48;
constexpr int COLLAPSE_TRIGGER_ICON_SIZE = 18;
constexpr auto DARK_COLLAPSE_TRIGGER_BACKGROUND = "#00203F";

QStandardItem* createActionItem(const QString& stableId, const QString& label,
                                const adqt::icons::IconRef& icon = adqt::icons::IconRef(),
                                bool selectable = true) {
    auto* item = new QStandardItem(label);
    item->setEditable(false);
    item->setSelectable(selectable);
    item->setData(label, Qt::DisplayRole);
    item->setData(label, Qt::ToolTipRole);
    item->setData(stableId, AdNavigationMenu::StableIdRole);
    item->setData(static_cast<int>(AdNavigationMenu::NodeKind::Action),
                  AdNavigationMenu::NodeKindRole);
    if (adqt::icons::isValid(icon)) {
        item->setData(QVariant::fromValue(icon), Qt::DecorationRole);
    }
    return item;
}

QModelIndex findIndexByStableId(const QAbstractItemModel* model, const QString& stableId,
                                const QModelIndex& parent = QModelIndex()) {
    if (model == nullptr || stableId.isEmpty()) {
        return QModelIndex();
    }

    const int rowCount = model->rowCount(parent);
    for (int row = 0; row < rowCount; ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (!index.isValid()) {
            continue;
        }

        if (index.data(AdNavigationMenu::StableIdRole).toString() == stableId) {
            return index;
        }

        if (const QModelIndex child = findIndexByStableId(model, stableId, index);
            child.isValid()) {
            return child;
        }
    }

    return QModelIndex();
}

void expandAncestors(AdNavigationMenu* menu, const QModelIndex& index) {
    if (menu == nullptr || !index.isValid()) {
        return;
    }

    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        menu->setExpanded(parent, true);
    }
}

void selectMenuIndex(AdNavigationMenu* menu, QItemSelectionModel* selectionModel,
                     const QModelIndex& index) {
    if (menu == nullptr || selectionModel == nullptr || !index.isValid()) {
        return;
    }

    expandAncestors(menu, index);
    selectionModel->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    selectionModel->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    menu->setCurrentIndex(index);
}
} // namespace

QString SidebarWidget::currentRoute() const {
    if (!m_currentRoute.isEmpty()) {
        return m_currentRoute;
    }

    const auto& catalog = m_registry.catalog();
    const auto* page = catalog.page(catalog.defaultLocation().pageId);
    return normalizeRouteKey(page != nullptr ? page->route : QStringLiteral("/"));
}

bool SidebarWidget::isCollapsed() const {
    return m_isCollapsed;
}

void SidebarWidget::setCurrentRoute(const QString& routeKey) {
    applyRouteSelection(routeKey);
}

void SidebarWidget::setCollapsed(bool collapsed) {
    const bool changed = m_isCollapsed != collapsed;
    m_isCollapsed = collapsed;

    syncCollapsedPresentation();

    if (m_menu != nullptr) {
        m_menu->setCollapsed(m_isCollapsed);
    }

    if (changed) {
        snow_shot::storage::InterfaceSettings().setSidebarCollapsed(m_isCollapsed);
        emit collapsedChanged(m_isCollapsed);
    }
}

QString SidebarWidget::normalizeRouteKey(const QString& routeKey) const {
    if (m_leafRoutes.contains(routeKey)) {
        return routeKey;
    }

    const auto& catalog = m_registry.catalog();
    const auto* defaultPage = catalog.page(catalog.defaultLocation().pageId);
    QString defaultRoute = defaultPage != nullptr ? defaultPage->route : QStringLiteral("/");
    if (m_leafRoutes.contains(defaultRoute)) {
        return defaultRoute;
    }

    if (!m_leafRoutes.isEmpty()) {
        return m_leafRoutes.constFirst();
    }

    return defaultRoute;
}

void SidebarWidget::rebuildNavigationModel() {
    if (m_menu == nullptr || m_menuModel == nullptr) {
        return;
    }

    m_menu->collapseAll();
    m_menuModel->clear();

    m_leafRoutes.clear();
    const auto appendPage = [this](QStandardItem* parent,
                                   const snow_shot::presentation::settings::SettingsNavigationPageDefinition& navigationPage) {
        const auto* page = m_registry.catalog().page(navigationPage.pageId);
        if (page == nullptr) {
            return;
        }
        m_leafRoutes.push_back(page->route);
        const adqt::icons::IconRef icon =
            parent == nullptr && navigationPage.iconFactory ? navigationPage.iconFactory()
                                                            : adqt::icons::IconRef();
        QStandardItem* item = createActionItem(page->route, page->title.translated(), icon);
        if (parent != nullptr) {
            parent->appendRow(item);
        } else {
            m_menuModel->appendRow(item);
        }
    };

    for (const auto& node : m_registry.navigation()) {
        if (const auto* navigationPage =
                std::get_if<snow_shot::presentation::settings::SettingsNavigationPageDefinition>(
                    &node)) {
            appendPage(nullptr, *navigationPage);
            continue;
        }
        const auto* group =
            std::get_if<snow_shot::presentation::settings::SettingsNavigationGroupDefinition>(
                &node);
        if (group == nullptr) {
            continue;
        }
        auto* groupItem = createActionItem(
            group->id, group->title.translated(),
            group->iconFactory ? group->iconFactory() : adqt::icons::IconRef(), false);
        for (const auto& navigationPage : group->pages) {
            appendPage(groupItem, navigationPage);
        }
        m_menuModel->appendRow(groupItem);
    }

    m_menu->setModel(m_menuModel);
    m_menu->setSelectionModel(m_menuSelectionModel);
    m_menu->setCurrentIndex(QModelIndex());
    m_menu->expandAll();
}

void SidebarWidget::applyRouteSelection(const QString& routeKey) {
    if (m_menu == nullptr || m_menuModel == nullptr || m_menuSelectionModel == nullptr) {
        m_currentRoute.clear();
        return;
    }

    const QString resolvedRouteKey = normalizeRouteKey(routeKey);
    const QModelIndex targetIndex = findIndexByStableId(m_menuModel, resolvedRouteKey);
    if (!targetIndex.isValid()) {
        m_currentRoute.clear();
        return;
    }

    selectMenuIndex(m_menu, m_menuSelectionModel, targetIndex);
    m_currentRoute = resolvedRouteKey;
}

void SidebarWidget::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    const QColor background = scheme.map.colorBgContainer;
    const QColor collapseTriggerBackground =
        scheme.appearance == snow_shot::presentation::styles::ThemeAppearance::Dark
            ? QColor(QString::fromLatin1(DARK_COLLAPSE_TRIGGER_BACKGROUND))
            : background;

    QPalette sidebarPalette = palette();
    sidebarPalette.setColor(QPalette::Window, background);
    setPalette(sidebarPalette);

    if (m_menu != nullptr) {
        QPalette palette = m_menu->palette();
        palette.setColor(QPalette::Window, background);
        m_menu->setPalette(palette);
    }

    if (m_collapseTrigger != nullptr) {
        QPalette palette = m_collapseTrigger->palette();
        palette.setColor(QPalette::Window, collapseTriggerBackground);
        m_collapseTrigger->setPalette(palette);
        m_collapseTrigger->setAutoFillBackground(true);
    }

    update();
}

SidebarWidget::SidebarWidget(
    const snow_shot::presentation::settings::SettingsRegistry& registry, QWidget* parent)
    : QFrame(parent), m_registry(registry) {
    setAutoFillBackground(true);

    auto* sidebarLayout = new QVBoxLayout(this);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    m_menu = new AdNavigationMenu(this);
    m_menu->setMode(AdNavigationMenu::Mode::Inline);
    m_menu->setAutoFillBackground(true);
    auto menuTokens = m_menu->componentTokens();
    menuTokens.metrics.rootPaddingBlockStart = FIRST_TOP_LEVEL_MENU_TOP_SPACING;
    m_menu->setComponentTokens(menuTokens);
    m_menu->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_menuModel = new QStandardItemModel(m_menu);
    m_menuSelectionModel = new QItemSelectionModel(m_menuModel, m_menu);

    rebuildNavigationModel();
    sidebarLayout->addWidget(m_menu, 1);

    m_collapseTrigger = new QFrame(this);
    m_collapseTrigger->setObjectName(QStringLiteral("sidebarCollapseTrigger"));
    m_collapseTrigger->setAutoFillBackground(true);
    m_collapseTrigger->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_collapseTrigger->setFixedHeight(COLLAPSE_TRIGGER_HEIGHT);

    auto* collapseTriggerLayout = new QVBoxLayout(m_collapseTrigger);
    collapseTriggerLayout->setContentsMargins(0, 0, 0, 0);
    collapseTriggerLayout->setSpacing(0);

    m_collapseButton = new adqt::widgets::AdButton(m_collapseTrigger);
    m_collapseButton->setObjectName(QStringLiteral("sidebarCollapseButton"));
    m_collapseButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    m_collapseButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    m_collapseButton->setShape(adqt::widgets::AdButton::Shape::Rectangle);
    m_collapseButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_collapseButton->setFixedHeight(COLLAPSE_TRIGGER_HEIGHT);
    m_collapseButton->setIconSize(QSize(COLLAPSE_TRIGGER_ICON_SIZE, COLLAPSE_TRIGGER_ICON_SIZE));
    m_collapseButton->setCursor(Qt::PointingHandCursor);
    collapseTriggerLayout->addWidget(m_collapseButton);
    sidebarLayout->addWidget(m_collapseTrigger);

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &SidebarWidget::applyTheme);
    applyTheme(themeManager.themeColorScheme());

    connect(m_collapseButton, &adqt::widgets::AdButton::clicked, this,
            [this]() { setCollapsed(!m_isCollapsed); });

    connect(m_menu, &AdNavigationMenu::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid() || m_menuModel == nullptr || m_menuModel->rowCount(index) > 0) {
            return;
        }

        const QString routeKey = index.data(AdNavigationMenu::StableIdRole).toString();
        if (!m_leafRoutes.contains(routeKey)) {
            return;
        }

        applyRouteSelection(routeKey);
        emit routeSelected(routeKey);
    });

    const auto& catalog = m_registry.catalog();
    const auto* defaultPage = catalog.page(catalog.defaultLocation().pageId);
    setCurrentRoute(defaultPage != nullptr ? defaultPage->route : QStringLiteral("/"));
    setCollapsed(snow_shot::storage::InterfaceSettings().sidebarCollapsed());
}

void SidebarWidget::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() != QEvent::LanguageChange) {
        return;
    }

    const QString route = m_currentRoute;
    rebuildNavigationModel();
    applyRouteSelection(route);
    syncCollapsedPresentation();
}

void SidebarWidget::syncCollapsedPresentation() {
    if (m_menu != nullptr) {
        const int width = m_isCollapsed ? SIDEBAR_COLLAPSED_WIDTH : SIDEBAR_EXPANDED_WIDTH;
        setFixedWidth(width);
    }

    if (m_collapseButton == nullptr) {
        return;
    }

    const bool expanding = m_isCollapsed;
    m_collapseButton->setIconRef(expanding ? outlined_icons::MenuUnfold()
                                           : outlined_icons::MenuFold());
    const QString actionText = expanding ? tr("Expand navigation") : tr("Collapse navigation");
    m_collapseButton->setToolTip(actionText);
    m_collapseButton->setAccessibleName(actionText);
}
