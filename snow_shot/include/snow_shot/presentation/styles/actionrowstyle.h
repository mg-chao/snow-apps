#ifndef SNOW_SHOT_PRESENTATION_STYLES_ACTIONROWSTYLE_H
#define SNOW_SHOT_PRESENTATION_STYLES_ACTIONROWSTYLE_H

#include "snow_shot/presentation/styles/buttonborder.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QPainter>

namespace snow_shot::presentation::styles {
inline QColor actionRowColor(const QString& state, bool pressed, bool hovered,
                             const ThemeMapColorToken& map, bool border = false) {
    if (pressed) {
        return map.colorPrimaryActive;
    }
    if (hovered) {
        return map.colorPrimaryHover;
    }
    if (state == QStringLiteral("focus")) {
        return map.colorPrimary;
    }
    if (state == QStringLiteral("highlight")) {
        return border ? map.colorPrimaryBorderHover : map.colorPrimaryHover;
    }
    return border ? map.colorBorder : map.colorText;
}

inline void paintActionRow(QPainter& painter, const QSize& size, const ThemeMapColorToken& map,
                           const QString& state, bool pressed, bool hovered, int radius,
                           int borderWidth, bool stableBorder) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(state == QStringLiteral("highlight") ? map.colorPrimaryBgHover
                                                          : map.colorBgContainer);
    painter.drawRoundedRect(QRectF(QPointF(), QSizeF(size)).adjusted(0.5, 0.5, -0.5, -0.5), radius,
                            radius);
    if (stableBorder) {
        ButtonBorderSpec spec;
        spec.color = actionRowColor(state, pressed, hovered, map, true);
        spec.width = borderWidth;
        spec.radius = radius;
        drawButtonBorder(&painter, size, spec);
    }
}
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_ACTIONROWSTYLE_H
