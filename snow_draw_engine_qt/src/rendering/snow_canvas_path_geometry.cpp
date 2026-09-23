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
