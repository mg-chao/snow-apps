#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_EMPTYSTATEICON_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_EMPTYSTATEICON_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "icon_renderer.h"
#include "icons/widget_icons.h"

#include <QColor>
#include <QPixmap>
#include <QSize>

namespace snow_shot::presentation::components {

inline QColor colorOnEmptyStateBackground(const QColor& foreground, const QColor& background) {
    if (!foreground.isValid()) {
        return background;
    }
    if (!background.isValid() || foreground.alpha() >= 255) {
        return foreground;
    }
    const float alpha = foreground.alphaF();
    return QColor::fromRgbF(foreground.redF() * alpha + background.redF() * (1.0F - alpha),
                            foreground.greenF() * alpha + background.greenF() * (1.0F - alpha),
                            foreground.blueF() * alpha + background.blueF() * (1.0F - alpha));
}

inline QPixmap renderEmptyStateIcon(const styles::ThemeColorScheme& scheme, const QSize& size,
                                    qreal devicePixelRatio) {
    const QColor background = scheme.map.colorBgContainer;
    return adqt::icons::renderIconPixmap(
        adqt::widgets::icons::twotone::EmptySimple(adqt::icons::IconColors::threeTone(
            colorOnEmptyStateBackground(scheme.map.colorFill, background),
            colorOnEmptyStateBackground(scheme.map.colorFillQuaternary, background),
            colorOnEmptyStateBackground(scheme.map.colorFillTertiary, background))),
        {size, devicePixelRatio});
}

} // namespace snow_shot::presentation::components

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_EMPTYSTATEICON_H
