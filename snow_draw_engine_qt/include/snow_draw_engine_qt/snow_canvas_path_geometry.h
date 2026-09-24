#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QVector>

// Stateless access to the engine's free-draw curve algorithm.
QPainterPath snowCanvasCatmullRomPath(const QVector<QPointF>& vertices, bool closed);

struct SnowStrokeFilter;

// Queues pointer events locally; crosses FFI once per preview batch, never resends history.
class SnowCanvasStrokeFilter {
  public:
    SnowCanvasStrokeFilter() = default;
    ~SnowCanvasStrokeFilter();
    SnowCanvasStrokeFilter(const SnowCanvasStrokeFilter&) = delete;
    SnowCanvasStrokeFilter& operator=(const SnowCanvasStrokeFilter&) = delete;

    void reset(const QPointF& start, qreal scale = 1.0);
    void append(const QPointF& point);
    QVector<QPointF> takePoints(bool finish = false);

  private:
    SnowStrokeFilter* m_filter = nullptr;
    QVector<QPointF> m_pending;
};
