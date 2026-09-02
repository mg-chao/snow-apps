#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_MAINCONTENTHEADERWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_MAINCONTENTHEADERWIDGET_H

#include "snow_shot/presentation/settings/settingsregistry.h"

#include <QFrame>
#include <QString>
#include <QVector>

class ApplicationSearchWidget;
class QEvent;
namespace adqt::widgets {
class AdTabs;
}
namespace snow_shot::presentation::styles {
struct ThemeAliasMetricToken;
struct ThemeColorScheme;
} // namespace snow_shot::presentation::styles
class MainContentHeaderWidget final : public QFrame {
    Q_OBJECT

  public:
    MainContentHeaderWidget(
        const snow_shot::presentation::settings::SettingsRegistry& registry,
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        QWidget* parent = nullptr);

    [[nodiscard]] QString currentSection() const;
    void setSections(
        const QVector<snow_shot::presentation::settings::SettingsSectionSummary>& sections);
    void setCurrentSection(const QString& sectionId);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();

  signals:
    void sectionRequested(const QString& sectionId);
    void locationRequested(
        const snow_shot::presentation::settings::SettingsLocation& location);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void updateLayoutMargins(
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric);

    adqt::widgets::AdTabs* m_tabs = nullptr;
    ApplicationSearchWidget* m_globalSearch = nullptr;
    QVector<snow_shot::presentation::settings::SettingsSectionSummary> m_sections;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_MAINCONTENTHEADERWIDGET_H
