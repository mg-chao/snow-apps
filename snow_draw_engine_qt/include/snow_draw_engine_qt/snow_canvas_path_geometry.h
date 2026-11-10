#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QVector>
#include "snow_draw_engine_qt/snow_canvas_types.h"

// Stateless access to the engine's free-draw curve algorithm.
QPainterPath snowCanvasCatmullRomPath(const QVector<QPointF>& vertices, bool closed);

// Vector ink coverage using the same dash and hand-drawn treatment as canvas lines.
// Geometry is independent of colour, opacity, viewport and pixel density.
QPainterPath snowCanvasStrokeCoverage(const QPainterPath& path, qreal width,
                                      SnowCanvasStrokeStyle pattern,
                                      SnowCanvasStrokeRenderingStyle rendering, quint32 seed);
// Maximum displacement of the hand-drawn centerline, before adding half the pen width.
qreal snowCanvasStrokeRenderingOutset(SnowCanvasStrokeRenderingStyle rendering);

struct SnowStrokeFilter;

// Queues pointer events locally; crosses FFI once per preview batch, never resends history.
class SnowCanvasStrokeFilter {
  public:
    SnowCanvasStrokeFilter() = default;
    ~SnowCanvasStrokeFilter();
    SnowCanvasStrokeFilter(const SnowCanvasStrokeFilter&) = delete;
    SnowCanvasStrokeFilter& operator=(const SnowCanvasStrokeFilter&) = delete;

    // Ends the stroke and releases native state and queued pointer-event storage.
    void clear() noexcept;
    void reset(const QPointF& start, qreal scale = 1.0);
    void append(const QPointF& point);
    QVector<QPointF> takePoints(bool finish = false);

  private:
    SnowStrokeFilter* m_filter = nullptr;
    QVector<QPointF> m_pending;
};
