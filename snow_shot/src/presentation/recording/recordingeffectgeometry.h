#pragma once
#include <QRectF>
#include <QTransform>

// Output pixels first map to the physical capture (which may include encoder padding),
// then to the exact selection's local coordinates. Canvas rounding is a separate offset.
inline QTransform recordingEffectsOutputTransform(const QRect& capture, const QRect& selected,
                                                  const QRectF& selectionInWindow,
                                                  const QPoint& canvasOrigin, qreal dpr,
                                                  const QSize& output) {
    const QPointF offset = selectionInWindow.topLeft() - canvasOrigin +
                           QPointF(capture.topLeft() - selected.topLeft()) / dpr;
    QTransform transform;
    transform.translate(offset.x(), offset.y());
    transform.scale(capture.width() / (dpr * output.width()),
                    capture.height() / (dpr * output.height()));
    return transform;
}
