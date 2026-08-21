#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_CONTENTCARDWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_CONTENTCARDWIDGET_H

#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QFrame>
#include <QHash>

#include <memory>

class QEvent;
class QPaintEvent;
class QStackedWidget;
class SettingsPageWidget;
class ScreenshotHistoryPageWidget;
class QWidget;
namespace snow_shot::presentation {
class GlobalShortcutManager;
}
namespace snow_shot::presentation::settings {
class SettingsRuntimeBindings;
struct SettingsPageDefinition;
}

class ContentCardWidget final : public QFrame {
    Q_OBJECT

  public:
    ContentCardWidget(const snow_shot::presentation::settings::SettingsCatalog& catalog,
                      snow_shot::presentation::GlobalShortcutManager& shortcutManager,
                      QWidget* parent = nullptr);
    ~ContentCardWidget() override;

    [[nodiscard]] QString currentRoute() const;
    [[nodiscard]] snow_shot::presentation::settings::SettingsLocation currentLocation() const;
    [[nodiscard]] QVector<snow_shot::presentation::settings::SettingsSectionSummary>
    currentSections() const;
    void setCurrentRoute(const QString& route);
    void activateSection(const QString& sectionId);
    void navigateTo(const snow_shot::presentation::settings::SettingsLocation& location);
    void showInterfaceSettings();
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();

  signals:
    void routeChanged(const QString& route);
    void sectionListChanged();
    void locationChanged(const snow_shot::presentation::settings::SettingsLocation& location);
    void screenshotRequested();
    void quickActionRequested(snow_shot::presentation::GlobalShortcutAction action);
    void screenshotHistoryEditRequested(const QString& recordId);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    QWidget* ensureRouteWidget(
        const snow_shot::presentation::settings::SettingsPageDefinition& pageDefinition);
    void handleCommand(const snow_shot::presentation::settings::SettingsCommand& command);

    const snow_shot::presentation::settings::SettingsCatalog& m_catalog;
    std::unique_ptr<snow_shot::presentation::settings::SettingsRuntimeBindings> m_runtimeBindings;
    QStackedWidget* m_stack = nullptr;
    QHash<QString, int> m_routePageIndices;
    QHash<QString, QWidget*> m_routeWidgetsById;
    QHash<QString, QWidget*> m_routePlaceholdersById;
    QHash<QString, SettingsPageWidget*> m_pagesById;
    ScreenshotHistoryPageWidget* m_historyPage = nullptr;
    snow_shot::presentation::settings::SettingsLocation m_currentLocation;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_CONTENTCARDWIDGET_H
