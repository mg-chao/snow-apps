#pragma once

#include <QRect>
#include <QRegion>

class SnowCanvasView;

namespace snow_canvas_widget_repaint {

QRegion clippedUpdateRegion(const QRegion& region, const QRect& clip);
QRegion adaptiveUpdateRegion(const QRegion& region, const QRect& clip,
                             double boundingAreaFactor = 1.5, int maximumRectCount = 256);
void updateClipped(SnowCanvasView& widget, const QRegion& region);
void updateCoalesced(SnowCanvasView& widget, const QRegion& region);

} // namespace snow_canvas_widget_repaint
