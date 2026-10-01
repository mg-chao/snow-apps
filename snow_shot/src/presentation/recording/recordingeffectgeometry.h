#pragma once
#include <QRectF>
#include <QTransform>

// Output pixels first map to native desktop capture units (pixels on Windows, points on macOS),
// then to the exact selection's local coordinates. Desktop scale is 1 for macOS points.
// Canvas rounding is a separate offset.
inline QTransform recordingEffectsOutputTransform(const QRect& capture, const QRect& selected,
                                                  const QRectF& selectionInWindow,
                                                  const QPoint& canvasOrigin, qreal desktopScale,
                                                  const QSize& output) {
    const QPointF offset = selectionInWindow.topLeft() - canvasOrigin +
                           QPointF(capture.topLeft() - selected.topLeft()) / desktopScale;
    QTransform transform;
    transform.translate(offset.x(), offset.y());
    transform.scale(capture.width() / (desktopScale * output.width()),
                    capture.height() / (desktopScale * output.height()));
    return transform;
}
