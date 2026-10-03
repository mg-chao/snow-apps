#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_MAINCONTENTHEADERWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_MAINCONTENTHEADERWIDGET_H

#include "snow_shot/presentation/settings/settingsregistry.h"

#include <QFrame>
#include <QColor>
#include <QString>
#include <QVector>

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
    explicit MainContentHeaderWidget(
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        QWidget* parent = nullptr);

    [[nodiscard]] QString currentSection() const;
    void
    setSections(const QVector<snow_shot::presentation::settings::SettingsSectionSummary>& sections);
    void setCurrentSection(const QString& sectionId);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void setSkinMaskOpacity(qreal opacity);

  signals:
    void sectionRequested(const QString& sectionId);

  private:
    void updateLayoutMargins(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric);
    void updateSkinMask();

    adqt::widgets::AdTabs* m_tabs = nullptr;
    QVector<snow_shot::presentation::settings::SettingsSectionSummary> m_sections;
    qreal m_skinMaskOpacity = 1.0;
    QColor m_surfaceColor;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_MAINCONTENTHEADERWIDGET_H
