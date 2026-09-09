#ifndef SNOW_SHOT_PRESENTATION_STYLES_ACTIONROWSTYLE_H
#define SNOW_SHOT_PRESENTATION_STYLES_ACTIONROWSTYLE_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "widgets/detail/button_rendering.h"

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
    if (stableBorder) {
        // Use AdButton's inset so the rounded backing-store clip cannot trim a stroke
        // at fractional display scales or child origins. Fill and stroke share one path.
        const QRectF borderRect = adqt::widgets::detail::joinedButtonBorderRect(
            QRect(QPoint(), size), borderWidth, false, false);
        const QPainterPath path =
            adqt::widgets::detail::roundedButtonPath(borderRect, radius, radius, radius, radius);
        painter.drawPath(path);
        if (borderWidth > 0) {
            painter.setPen(adqt::widgets::detail::makeButtonBorderPen(
                actionRowColor(state, pressed, hovered, map, true), borderWidth, Qt::SolidLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        }
        return;
    }
    painter.drawRoundedRect(QRectF(QPointF(), QSizeF(size)).adjusted(0.5, 0.5, -0.5, -0.5), radius,
                            radius);
}
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_ACTIONROWSTYLE_H
