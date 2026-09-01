#pragma once

#include <QRect>
#include <QRegion>

class QWidget;

namespace snow_canvas_widget_repaint {

QRegion clippedUpdateRegion(const QRegion& region, const QRect& clip);
QRegion adaptiveUpdateRegion(const QRegion& region, const QRect& clip,
                             double boundingAreaFactor = 1.5, int maximumRectCount = 256);
void updateClipped(QWidget& widget, const QRegion& region);
void updateCoalesced(QWidget& widget, const QRegion& region);

} // namespace snow_canvas_widget_repaint
