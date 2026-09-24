#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"

#include "snow_draw_engine.h"

#include <vector>

QPainterPath snowCanvasCatmullRomPath(const QVector<QPointF>& vertices, bool closed) {
    std::vector<SnowArrowPoint> points;
    points.reserve(static_cast<std::size_t>(vertices.size()));
    for (const auto& vertex : vertices)
        points.push_back({vertex.x(), vertex.y()});
    std::vector<SnowArrowPathCommand> commands(points.size() + 1);
    std::size_t count = 0;
    QPainterPath path;
    if (snow_build_catmull_rom_path(points.data(), points.size(), closed ? 1 : 0, commands.data(),
                                    commands.size(), &count) != SNOW_OK)
        return path;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& c = commands[i];
        const QPointF point(c.point.x, c.point.y);
        switch (c.kind) {
        case SNOW_ARROW_PATH_COMMAND_MOVE_TO:
            path.moveTo(point);
            break;
        case SNOW_ARROW_PATH_COMMAND_LINE_TO:
            path.lineTo(point);
            break;
        case SNOW_ARROW_PATH_COMMAND_CUBIC_TO:
            path.cubicTo(QPointF(c.control1.x, c.control1.y), QPointF(c.control2.x, c.control2.y),
                         point);
            break;
        default:
            break;
        }
    }
    if (closed && !path.isEmpty())
        path.closeSubpath();
    path.setFillRule(Qt::OddEvenFill);
    return path;
}

SnowCanvasStrokeFilter::~SnowCanvasStrokeFilter() {
    snow_stroke_filter_free(m_filter);
}

void SnowCanvasStrokeFilter::reset(const QPointF& start, qreal scale) {
    snow_stroke_filter_free(m_filter);
    m_filter = nullptr;
    m_pending.clear();
    const auto error =
        snow_stroke_filter_create({start.x(), start.y()}, 0.75 / scale, 6.0 / scale, &m_filter);
    Q_ASSERT(error == SNOW_OK);
    Q_UNUSED(error);
}

void SnowCanvasStrokeFilter::append(const QPointF& point) {
    m_pending.append(point);
}

QVector<QPointF> SnowCanvasStrokeFilter::takePoints(bool finish) {
    if (!m_filter || (m_pending.isEmpty() && !finish))
        return {};
    std::vector<SnowArrowPoint> input;
    input.reserve(static_cast<std::size_t>(m_pending.size()));
    for (const auto& point : m_pending)
        input.push_back({point.x(), point.y()});
    const SnowArrowPoint* output = nullptr;
    std::size_t count = 0;
    const auto error = snow_stroke_filter_append(m_filter, input.data(), input.size(),
                                                 finish ? 1 : 0, &output, &count);
    Q_ASSERT(error == SNOW_OK);
    m_pending.clear();
    QVector<QPointF> points;
    if (error != SNOW_OK)
        return points;
    points.reserve(static_cast<qsizetype>(count));
    for (std::size_t i = 0; i < count; ++i)
        points.append(QPointF(output[i].x, output[i].y));
    return points;
}
