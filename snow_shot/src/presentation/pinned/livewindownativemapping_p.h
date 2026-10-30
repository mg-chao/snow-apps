// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_LIVEWINDOWNATIVEMAPPING_P_H
#define SNOW_SHOT_PRESENTATION_LIVEWINDOWNATIVEMAPPING_P_H
#include <QRectF>
#include <QTransform>
#include <optional>

namespace snow_shot::presentation::live_window_detail {
// Source/window and crop coordinates use desktop points; destination uses view
// points. Backing scale belongs to the native layer, not to this geometry.
inline std::optional<QTransform>
nativeFramePlacement(const QRectF& crop, const QTransform& transform, const QSizeF& viewport) {
    if (crop.isEmpty() || viewport.isEmpty() || !transform.isAffine() || !transform.isInvertible())
        return {};
    const QRectF oriented = transform.mapRect(QRectF(QPointF(), crop.size()));
    if (oriented.isEmpty())
        return {};
    QTransform local;
    local.translate(-crop.x(), -crop.y());
    QTransform placement;
    placement.scale(viewport.width() / oriented.width(), viewport.height() / oriented.height());
    placement.translate(-oriented.x(), -oriented.y());
    return local * transform * placement;
}
} // namespace snow_shot::presentation::live_window_detail
#endif
