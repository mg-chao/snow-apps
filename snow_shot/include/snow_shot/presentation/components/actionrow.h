#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_ACTIONROW_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_ACTIONROW_H

#include "icon_core.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "widgets/button.h"

class QLabel;
class QHBoxLayout;
namespace snow_shot::presentation::styles {
struct MainWindowComponentMetricToken;
}

struct ActionRowConfig {
    QString title;
    adqt::icons::IconRef iconRef;
    QString rowState;
    bool useStableBorder = false;
    bool compactPresentation = false;
    bool interactiveTitle = false;
};

// Shared action surface; subclasses own only their configuration and action behavior.
class ActionRow : public adqt::widgets::AdButton {
    Q_OBJECT

  public:
    ActionRow(
        const ActionRowConfig& config,
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        const snow_shot::presentation::styles::MainWindowComponentMetricToken& mainWindowMetric,
        const snow_shot::presentation::styles::ThemeColorScheme& scheme, QWidget* parent = nullptr);
    virtual void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);

  protected:
    void setConfigurationButton(adqt::widgets::AdButton* button);
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    QLabel* m_titleLabel = nullptr;
    bool m_compactPresentation = false;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;

  private:
    void syncTitle();
    [[nodiscard]] bool isConfigurationButtonActive() const;

    QHBoxLayout* m_rowLayout = nullptr;
    QLabel* m_titleIcon = nullptr;
    adqt::widgets::AdButton* m_configurationButton = nullptr;
    adqt::icons::IconRef m_titleIconRef;
    QString m_rowState;
    int m_titleIconSize = 0;
    int m_rowBorderWidth = 1;
    int m_rowBorderRadius = 0;
    bool m_useStableBorder = false;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_ACTIONROW_H
