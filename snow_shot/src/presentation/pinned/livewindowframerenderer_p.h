// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_LIVEWINDOWFRAMERENDERER_P_H
#define SNOW_SHOT_PRESENTATION_LIVEWINDOWFRAMERENDERER_P_H

#include <QImage>
#include <QPainter>
#include <QTransform>

namespace snow_shot::presentation::live_window_detail {

inline QSize transformedFrameSize(const QSize& pixels, const QTransform& transform) {
    const auto matrix = QImage::trueMatrix(transform, pixels.width(), pixels.height());
    return matrix.mapRect(QRect(QPoint(), pixels)).size();
}

// Live display keeps capture pixels immutable. Rotate/reflect while sampling the
// destination, so a display update never allocates an intermediate rotated image.
// The target and the caller's clip are in painter coordinates, independent of DPR.
inline void paintLiveWindowFrame(QPainter& painter, const QRectF& target, const QImage& frame,
                                 const QTransform& transform, bool linearFiltering) {
    if (frame.isNull() || target.isEmpty())
        return;
    const auto matrix = QImage::trueMatrix(transform, frame.width(), frame.height());
    const QRectF bounds = matrix.mapRect(QRectF(frame.rect()));
    if (bounds.isEmpty())
        return;
    QTransform placement;
    placement.translate(target.x(), target.y());
    placement.scale(target.width() / bounds.width(), target.height() / bounds.height());
    placement.translate(-bounds.x(), -bounds.y());
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, linearFiltering);
    painter.setTransform(matrix * placement, true);
    // Explicit pixel rectangles avoid interpreting capture DPR as a logical size.
    painter.drawImage(QRectF(frame.rect()), frame, QRectF(frame.rect()));
    painter.restore();
}

} // namespace snow_shot::presentation::live_window_detail
#endif
