#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SHORTCUTCONFIGURATIONBUTTON_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SHORTCUTCONFIGURATIONBUTTON_H

#include "icon_core.h"
#include "snow_shot/presentation/globalshortcuttypes.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "widgets/button.h"

class QEvent;
class QPaintEvent;
class InfoTooltipIcon;

class ShortcutConfigurationButton final : public adqt::widgets::AdButton {
    Q_OBJECT

  public:
    explicit ShortcutConfigurationButton(
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, int textMaxWidth,
        QWidget* parent = nullptr);
    ShortcutConfigurationButton(
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, int textMaxWidth,
        const adqt::icons::IconRef& contentIcon, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    void setRegistrationStatus(snow_shot::presentation::GlobalShortcutStatus status);
    void setRegistrationStatusTooltipVisible(bool visible);
    [[nodiscard]] InfoTooltipIcon* registrationStatusTooltipTrigger() const;
    void setTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);

  protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    [[nodiscard]] int statusTooltipReservationWidth() const;
    void syncStatusTooltipTrigger();

    int m_iconTextSpacing = 6;
    int m_textMaxWidth = 200;
    InfoTooltipIcon* m_statusTooltipTrigger = nullptr;
    bool m_statusTooltipVisible = false;
    bool m_hovered = false;
    snow_shot::presentation::GlobalShortcutStatus m_status =
        snow_shot::presentation::GlobalShortcutStatus::Unset;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SHORTCUTCONFIGURATIONBUTTON_H
