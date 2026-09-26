#pragma once

#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QPainterPath>
#include <QPointF>
#include <QPointer>
#include <QTimer>

#include <deque>
#include <functional>
#include <optional>

class SnowCanvasWidget;

namespace snow_shot::presentation {

// A transient presentation layer. The host controls canvas interaction and must
// detach this borrowed custom renderer before destroying it.
class FullscreenCanvasLaser final : public QObject, public SnowCanvasCustomRenderer {
  public:
    explicit FullscreenCanvasLaser(SnowCanvasWidget& canvas, QObject* parent = nullptr,
                                   std::function<qint64()> clock = {});

    void setActive(bool active);
    [[nodiscard]] bool active() const;
    void setStyle(const QColor& color, qreal width, int durationMs);
    void clear();
    [[nodiscard]] bool trailEmpty() const;
    [[nodiscard]] bool animationActive() const;
    void advance();

    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Sample {
        QPointF position;
        qint64 timestamp = 0;
        bool connected = false;
    };

    void appendPoint(const QPointF& position, bool start);
    void prune(qint64 now);
    void rebuildPath(qint64 now);
    void repaintDamage(const QRectF& previous);
    [[nodiscard]] qint64 currentTime() const;

    QPointer<SnowCanvasWidget> m_canvas;
    QElapsedTimer m_elapsed;
    std::function<qint64()> m_clock;
    QTimer m_timer;
    std::deque<Sample> m_samples;
    std::optional<QPointF> m_lastPosition;
    QPainterPath m_path;
    QColor m_color = Qt::red;
    qreal m_width = 4.0;
    int m_durationMs = 1000;
    bool m_active = false;
    bool m_drawing = false;
};

} // namespace snow_shot::presentation
