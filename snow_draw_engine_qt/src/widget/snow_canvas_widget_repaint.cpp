#include "snow_canvas_widget_repaint.h"

#include <QWidget>

namespace snow_canvas_widget_repaint {

QRegion clippedUpdateRegion(const QRegion& region, const QRect& clip) {
    if (region.isEmpty()) {
        return {};
    }
    return region.intersected(clip);
}


QRegion adaptiveUpdateRegion(const QRegion& region, const QRect& clip, double boundingAreaFactor,
                             int maximumRectCount) {
    QRegion clipped = clippedUpdateRegion(region, clip);
    if (clipped.isEmpty()) {
        return {};
    }
    const QRect bounds = clipped.boundingRect();
    qint64 regionArea = 0;
    int rectCount = 0;
    for (const QRect& rect : clipped) {
        regionArea += static_cast<qint64>(rect.width()) * rect.height();
        ++rectCount;
    }
    const qint64 boundingArea = static_cast<qint64>(bounds.width()) * bounds.height();
    if (rectCount > maximumRectCount ||
        static_cast<double>(boundingArea) <= static_cast<double>(regionArea) * boundingAreaFactor) {
        return QRegion(bounds);
    }
    return clipped;
}

void updateClipped(QWidget& widget, const QRegion& region) {
    const QRegion updateRegion = clippedUpdateRegion(region, widget.rect());
    if (!updateRegion.isEmpty()) {
        widget.update(updateRegion);
    }
}

void updateCoalesced(QWidget& widget, const QRegion& region) {
    const QRegion updateRegion = adaptiveUpdateRegion(region, widget.rect());
    if (!updateRegion.isEmpty()) {
        widget.update(updateRegion);
    }
}

} // namespace snow_canvas_widget_repaint
