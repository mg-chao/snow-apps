// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTHIDPIIMAGESCALING_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTHIDPIIMAGESCALING_H

#include <QRectF>
#include <QSize>
#include <QStringView>
#include <QVector>

#include <algorithm>
#include <cmath>

// All bounds in a snapshot use the same coordinates as the capture region.
struct ScreenshotHiDpiDisplay final {
    QRectF bounds;
    qreal scale = 0.0;
};

[[nodiscard]] inline qreal
screenshotHiDpiImageScale(QStringView mode, const QRectF& region,
                          const QVector<ScreenshotHiDpiDisplay>& displays) {
    if (mode == u"auto") {
        qreal commonScale = 0.0;
        for (const auto& display : displays) {
            if (!region.intersects(display.bounds))
                continue;
            if (!std::isfinite(display.scale) || display.scale <= 0.0 ||
                (commonScale > 0.0 && !qFuzzyCompare(commonScale, display.scale)))
                return 1.0;
            commonScale = display.scale;
        }
        return commonScale > 1.0 ? 1.0 / commonScale : 1.0;
    }
    bool valid = false;
    const int percentage = mode.toInt(&valid);
    if (!valid || percentage < 125 || percentage > 350 || percentage % 25 != 0)
        return 1.0;
    return 100.0 / percentage;
}

[[nodiscard]] inline QSize screenshotHiDpiImageSize(QSize size, qreal scale) {
    if (size.isEmpty() || !std::isfinite(scale) || scale <= 0.0 || scale > 1.0)
        return {};
    return {std::max(1, qRound(size.width() * scale)), std::max(1, qRound(size.height() * scale))};
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTHIDPIIMAGESCALING_H
