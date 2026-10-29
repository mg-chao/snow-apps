// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTLIVEWINDOWREQUEST_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTLIVEWINDOWREQUEST_H

#include "snow_shot/presentation/livewindowsource.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#include <array>
#include <cmath>

struct ScreenshotLiveWindowRequest {
    snow_shot::presentation::LiveWindowTarget target;
    QRect nativeSelection;
    QPointer<QScreen> screen;
};

// The selection is its unedited canvas bounding rectangle. Native live capture
// owns intersection with the actual capturable window bounds after initialization.
template <typename Lookup>
[[nodiscard]] std::optional<ScreenshotLiveWindowRequest>
screenshotLiveWindowRequest(const QRect& selection, const ScreenshotDisplaySession& displays,
                            const ScreenshotGeometryMapper& geometry,
                            const QVector<quintptr>& excluded, Lookup lookup) {
    if (selection.isEmpty() || displays.hasImageSources())
        return std::nullopt;
    const QPointF center = ScreenshotHalfOpenRect::fromRect(selection).center();
    const std::array<QPoint, 5> probes{
        QPoint(static_cast<int>(std::floor(center.x())), static_cast<int>(std::floor(center.y()))),
        selection.topLeft(), selection.topRight(), selection.bottomRight(), selection.bottomLeft()};
    for (const QPoint& probe : probes) {
        const auto* display = geometry.displayForCanvasPoint(displays, probe);
        // A display gap must never become a hit on the nearest display.
        if (!display)
            continue;
        const QPoint nativePoint =
            display->canvasUsesPoints
                ? geometry.logicalPositionForCanvasPoint(*display, probe).toPoint()
                : geometry.physicalPositionForCanvasPoint(displays, probe);
        const auto target = lookup(nativePoint, excluded);
        if (target && target->nativeId != 0)
            return ScreenshotLiveWindowRequest{
                *target, selection.translated(geometry.canvasOrigin()), display->screen};
    }
    return std::nullopt;
}

#endif
