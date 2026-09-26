#include "fullscreencanvaslaser.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <utility>

namespace snow_shot::presentation {
namespace {
constexpr std::size_t kMaximumSamples = 256;
constexpr qreal kTaperSampleCount = 50.0;
constexpr int kCurveSteps = 8;

struct Vertex {
    QPointF position;
    qreal radius = 0.0;
};

Vertex midpoint(const Vertex& first, const Vertex& second) {
    return {(first.position + second.position) * 0.5, (first.radius + second.radius) * 0.5};
}

void addSegment(QPainterPath& path, const Vertex& first, const Vertex& second) {
    if (first.radius <= 0.0 && second.radius <= 0.0) {
        return;
    }
    if (first.radius > 0.0) {
        path.addEllipse(first.position, first.radius, first.radius);
    }
    if (second.radius > 0.0) {
        path.addEllipse(second.position, second.radius, second.radius);
    }

    const QPointF delta = second.position - first.position;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length <= 0.001) {
        return;
    }
    const QPointF normal(-delta.y() / length, delta.x() / length);
    // Match addEllipse's winding so overlapping round segments form one fill,
    // including when the chosen laser color is translucent.
    path.addPolygon(QPolygonF{
        first.position - normal * first.radius, second.position - normal * second.radius,
        second.position + normal * second.radius, first.position + normal * first.radius});
    path.closeSubpath();
}

void addCurve(QPainterPath& path, const Vertex& first, const Vertex& control, const Vertex& last) {
    Vertex previous = first;
    for (int step = 1; step <= kCurveSteps; ++step) {
        const qreal t = static_cast<qreal>(step) / kCurveSteps;
        const qreal inverse = 1.0 - t;
        const Vertex next{first.position * (inverse * inverse) +
                              control.position * (2.0 * inverse * t) + last.position * (t * t),
                          first.radius * (inverse * inverse) +
                              control.radius * (2.0 * inverse * t) + last.radius * (t * t)};
        addSegment(path, previous, next);
        previous = next;
    }
}
} // namespace

FullscreenCanvasLaser::FullscreenCanvasLaser(SnowCanvasWidget& canvas, QObject* parent,
                                             std::function<qint64()> clock)
    : QObject(parent), m_canvas(&canvas), m_clock(std::move(clock)) {
    m_elapsed.start();
    m_timer.setInterval(16);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &FullscreenCanvasLaser::advance);
    connect(&canvas, &QObject::destroyed, this, [this] { clear(); });
    canvas.installEventFilter(this);
}

void FullscreenCanvasLaser::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!active) {
        clear();
    }
}

bool FullscreenCanvasLaser::active() const {
    return m_active;
}

void FullscreenCanvasLaser::setStyle(const QColor& color, qreal width, int durationMs) {
    m_color = color.isValid() ? color : QColor(Qt::red);
    m_width = std::isfinite(width) ? std::clamp(width, 1.0, 20.0) : 4.0;
    m_durationMs = std::clamp(durationMs, 100, 5000);
    advance();
}

void FullscreenCanvasLaser::clear() {
    const QRectF previous = m_path.boundingRect();
    m_timer.stop();
    m_samples.clear();
    m_lastPosition.reset();
    m_path = {};
    m_drawing = false;
    repaintDamage(previous);
}

bool FullscreenCanvasLaser::trailEmpty() const {
    return m_samples.empty();
}

bool FullscreenCanvasLaser::animationActive() const {
    return m_timer.isActive();
}

qint64 FullscreenCanvasLaser::currentTime() const {
    return m_clock ? m_clock() : m_elapsed.elapsed();
}

void FullscreenCanvasLaser::prune(qint64 now) {
    // Preserve one expired predecessor for a continuous taper at the time boundary.
    while (m_samples.size() > 1 && now - m_samples[1].timestamp >= m_durationMs) {
        m_samples.pop_front();
    }
    if (!m_samples.empty() && now - m_samples.back().timestamp >= m_durationMs) {
        m_samples.clear();
    }
    if (!m_samples.empty()) {
        m_samples.front().connected = false;
    }
}

void FullscreenCanvasLaser::appendPoint(const QPointF& position, bool start) {
    if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
        (!start && m_lastPosition == position)) {
        return;
    }
    const qint64 now = currentTime();
    prune(now);
    const bool connected = !start && !m_samples.empty();
    const QPointF smoothed =
        connected ? m_samples.back().position + (position - m_samples.back().position) * 0.6
                  : position;
    m_lastPosition = position;
    m_samples.push_back({smoothed, now, connected});
    if (m_samples.size() > kMaximumSamples) {
        m_samples.pop_front();
        m_samples.front().connected = false;
    }
    advance();
}

void FullscreenCanvasLaser::advance() {
    const QRectF previous = m_path.boundingRect();
    const qint64 now = currentTime();
    prune(now);
    rebuildPath(now);
    repaintDamage(previous);
    if (m_samples.empty()) {
        m_timer.stop();
    } else if (!m_timer.isActive()) {
        m_timer.start();
    }
}

void FullscreenCanvasLaser::rebuildPath(qint64 now) {
    m_path = {};
    m_path.setFillRule(Qt::WindingFill);
    std::size_t start = 0;
    while (start < m_samples.size()) {
        std::size_t end = start + 1;
        while (end < m_samples.size() && m_samples[end].connected) {
            ++end;
        }
        const auto vertex = [this, now, end](std::size_t index) {
            const Sample& sample = m_samples[index];
            const qreal elapsed =
                std::clamp(static_cast<qreal>(now - sample.timestamp) / m_durationMs, 0.0, 1.0);
            const qreal distance =
                std::min(1.0, static_cast<qreal>(end - index - 1) / kTaperSampleCount);
            const qreal taper =
                std::min(1.0 - std::pow(elapsed, 4.0), 1.0 - std::pow(distance, 4.0));
            return Vertex{sample.position, m_width * 0.5 * taper};
        };
        if (end - start >= 2) {
            Vertex from = vertex(start);
            for (std::size_t index = start + 1; index + 1 < end; ++index) {
                const Vertex control = vertex(index);
                const Vertex to = midpoint(control, vertex(index + 1));
                addCurve(m_path, from, control, to);
                from = to;
            }
            addSegment(m_path, from, vertex(end - 1));
        }
        start = end;
    }
}

void FullscreenCanvasLaser::repaintDamage(const QRectF& previous) {
    if (m_canvas == nullptr) {
        return;
    }
    QRegion damage;
    if (!previous.isEmpty()) {
        damage += previous.adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect();
    }
    if (!m_path.isEmpty()) {
        damage += m_path.boundingRect().adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect();
    }
    if (!damage.isEmpty()) {
        m_canvas->update(damage.intersected(m_canvas->rect()));
    }
}

void FullscreenCanvasLaser::renderAfterCanvas(QPainter& painter,
                                              const SnowCanvasRenderContext& context) {
    if (m_path.isEmpty()) {
        return;
    }
    painter.save();
    painter.setClipRegion(context.exposedRegion.intersected(context.viewportRect),
                          Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(m_path, m_color);
    painter.restore();
}

bool FullscreenCanvasLaser::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_canvas || !m_active) {
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_drawing = true;
            appendPoint(mouse->position(), true);
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (m_drawing && mouse->buttons().testFlag(Qt::LeftButton)) {
            appendPoint(mouse->position(), false);
            return true;
        }
        m_drawing = false;
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && m_drawing) {
            appendPoint(mouse->position(), false);
            m_drawing = false;
            return true;
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::FocusOut:
    case QEvent::Hide:
        m_drawing = false;
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace snow_shot::presentation
