#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SIDEBARWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SIDEBARWIDGET_H

#include <QFrame>
#include <QStringList>

#include "snow_shot/presentation/settings/settingsregistry.h"

class QItemSelectionModel;
class QEvent;
class QString;
class QStandardItemModel;
class QWidget;
namespace adqt::widgets {
class AdButton;
class AdNavigationMenu;
} // namespace adqt::widgets
namespace snow_shot::presentation::styles {
struct ThemeColorScheme;
}
class SidebarWidget : public QFrame {
    Q_OBJECT

  public:
    explicit SidebarWidget(const snow_shot::presentation::settings::SettingsRegistry& registry,
                           QWidget* parent = nullptr);
    [[nodiscard]] QString currentRoute() const;
    [[nodiscard]] bool isCollapsed() const;
    void setCurrentRoute(const QString& routeKey);
    void setCollapsed(bool collapsed);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);

  signals:
    void collapsedChanged(bool collapsed);
    void routeSelected(const QString& routeKey);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void rebuildNavigationModel();
    void applyRouteSelection(const QString& routeKey);
    QString normalizeRouteKey(const QString& routeKey) const;
    void syncCollapsedPresentation();

    QString m_currentRoute;
    QStringList m_leafRoutes;
    bool m_isCollapsed = false;
    QFrame* m_collapseTrigger = nullptr;
    adqt::widgets::AdButton* m_collapseButton = nullptr;
    adqt::widgets::AdNavigationMenu* m_menu = nullptr;
    QStandardItemModel* m_menuModel = nullptr;
    QItemSelectionModel* m_menuSelectionModel = nullptr;
    const snow_shot::presentation::settings::SettingsRegistry& m_registry;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SIDEBARWIDGET_H
