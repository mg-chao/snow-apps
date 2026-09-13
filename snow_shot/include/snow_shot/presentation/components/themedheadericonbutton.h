#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_THEMEDHEADERICONBUTTON_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_THEMEDHEADERICONBUTTON_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "icon_core.h"
#include "widgets/button.h"

#include <algorithm>

class ThemedHeaderIconButton final : public adqt::widgets::AdButton {
  public:
    ThemedHeaderIconButton(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                           const adqt::icons::IconRef& iconRef, QWidget* parent = nullptr)
        : adqt::widgets::AdButton(parent) {
        const int iconSize = std::max(metric.fontSize, metric.controlInteractiveSize);
        const int buttonSize = std::max(metric.controlHeight, iconSize + metric.paddingXXS * 2);
        setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
        setShape(adqt::widgets::AdButton::Shape::Circle);
        setIconRef(iconRef);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setFixedSize(buttonSize, buttonSize);
    }
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_THEMEDHEADERICONBUTTON_H
