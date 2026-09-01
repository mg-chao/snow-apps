#include "snow_canvas_render_geometry.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLineF>
#include <QString>

#include <algorithm>
#include <cmath>

namespace snow_canvas_render_geometry {
namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr double kTextLineHeightPerFontSize = 1.2;
constexpr double kTextBackgroundHorizontalPaddingPerLineHeight = 0.32;
constexpr double kTextBackgroundVerticalPaddingPerLineHeight = 0.1;
struct CurveSegment {
    QPointF start;
    QPointF control1;
    QPointF control2;
    QPointF end;
};

QVector<CurveSegment> curveSegmentsForPoints(const QVector<QPointF>& points) {
    QVector<CurveSegment> segments;
    if (points.size() < 2) {
        return segments;
    }

    segments.reserve(points.size() - 1);
    for (int index = 0; index < points.size() - 1; ++index) {
        const QPointF previous = index == 0 ? points[index] : points[index - 1];
        const QPointF start = points[index];
        const QPointF end = points[index + 1];
        const QPointF next = index + 2 < points.size() ? points[index + 2] : end;
        segments.push_back(CurveSegment{
            start,
            QPointF(start.x() + (end.x() - previous.x()) / 6.0,
                    start.y() + (end.y() - previous.y()) / 6.0),
            QPointF(end.x() - (next.x() - start.x()) / 6.0, end.y() - (next.y() - start.y()) / 6.0),
            end,
        });
    }
    return segments;
}

QPointF pointAtBezier(const CurveSegment& segment, double t) {
    const double oneMinusT = 1.0 - t;
    return QPointF(oneMinusT * oneMinusT * oneMinusT * segment.start.x() +
                       3.0 * oneMinusT * oneMinusT * t * segment.control1.x() +
                       3.0 * oneMinusT * t * t * segment.control2.x() + t * t * t * segment.end.x(),
                   oneMinusT * oneMinusT * oneMinusT * segment.start.y() +
                       3.0 * oneMinusT * oneMinusT * t * segment.control1.y() +
                       3.0 * oneMinusT * t * t * segment.control2.y() +
                       t * t * t * segment.end.y());
}

QPainterPath elbowArrowPath(const QVector<QPointF>& points, double radius) {
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }

    path.moveTo(points.first());
    if (points.size() == 1) {
        return path;
    }

    for (int index = 1; index < points.size() - 1; ++index) {
        const QPointF prev = points[index - 1];
        const QPointF point = points[index];
        const QPointF next = points[index + 1];
        const bool prevHorizontal = std::abs(point.x() - prev.x()) > std::abs(point.y() - prev.y());
        const bool nextHorizontal = std::abs(next.x() - point.x()) > std::abs(next.y() - point.y());
        const double corner = qMin(
            radius, qMin(QLineF(point, prev).length() / 2.0, QLineF(point, next).length() / 2.0));

        QPointF entry = point;
        QPointF exit = point;
        if (prevHorizontal) {
            entry.setX(prev.x() < point.x() ? point.x() - corner : point.x() + corner);
        } else {
            entry.setY(prev.y() < point.y() ? point.y() - corner : point.y() + corner);
        }
        if (nextHorizontal) {
            exit.setX(next.x() < point.x() ? point.x() - corner : point.x() + corner);
        } else {
            exit.setY(next.y() < point.y() ? point.y() - corner : point.y() + corner);
        }

        path.lineTo(entry);
        path.quadTo(point, exit);
    }

    path.lineTo(points.last());
    return path;
}

QPainterPath curveArrowPath(const QVector<QPointF>& points) {
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }

    path.moveTo(points.first());
    const QVector<CurveSegment> segments = curveSegmentsForPoints(points);
    if (segments.isEmpty()) {
        return path;
    }

    for (const CurveSegment& segment : segments) {
        path.cubicTo(segment.control1, segment.control2, segment.end);
    }

    return path;
}

double scaleConstraint(double limit, double sum) {
    if (sum <= 0.0) {
        return 1.0;
    }
    return qMin(limit / sum, 1.0);
}

QRectF snapGuideLabelBounds(SnowSnapGuideAxis axis, const QPointF& start, const QPointF& end,
                            double label) {
    const QString text = QString::number(label, 'f', 0);
    QFont labelFont;
    labelFont.setPointSizeF(10.0);
    const QFontMetricsF metrics(labelFont);
    const QPointF midpoint((start.x() + end.x()) / 2.0, (start.y() + end.y()) / 2.0);

    QPointF textOrigin;
    if (axis == SNOW_SNAP_GUIDE_HORIZONTAL) {
        textOrigin =
            QPointF(midpoint.x() - metrics.horizontalAdvance(text) / 2.0, midpoint.y() - 6.0);
    } else {
        textOrigin = QPointF(midpoint.x() + 6.0, midpoint.y() - metrics.height() / 2.0);
    }

    QRectF textBounds = metrics.tightBoundingRect(text);
    textBounds.translate(textOrigin);
    return textBounds.adjusted(-2.0, -2.0, 2.0, 2.0);
}

QRectF overlayRectBounds(const ViewProjection& projection, const SnowOverlayDisplayItem& item) {
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    return rotatedRectBounds(center, item.width * projection.cameraZoom,
                             item.height * projection.cameraZoom, item.rotation,
                             item.stroke_width * projection.cameraZoom);
}

QRectF textItemBounds(const ViewProjection& projection, const SnowSceneDisplayItem& item) {
    const bool canPaintText = item.text_utf8_len != 0 && item.font_size > 0.0;
    const double strokeOutset =
        canPaintText && item.stroke.a != 0 ? qMax(0.0, item.stroke_width) / 2.0 : 0.0;
    double fillOutsetX = 0.0;
    double fillOutsetY = 0.0;
    if (item.fill.a != 0 && item.font_size > 0.0) {
        const double lineHeight = item.font_size * kTextLineHeightPerFontSize;
        fillOutsetX = lineHeight * kTextBackgroundHorizontalPaddingPerLineHeight;
        fillOutsetY = lineHeight * kTextBackgroundVerticalPaddingPerLineHeight;
    }

    const double outsetX = qMax(strokeOutset, fillOutsetX);
    const double outsetY = qMax(strokeOutset, fillOutsetY);
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    return rotatedRectBounds(center, (item.width + outsetX * 2.0) * projection.cameraZoom,
                             (item.height + outsetY * 2.0) * projection.cameraZoom, item.rotation,
                             0.0);
}

QRectF snapGuideBounds(const ViewProjection& projection, const SnowOverlayDisplayItem& item) {
    const QPointF start = canvasToView(projection, item.snap_start_x, item.snap_start_y);
    const QPointF end = canvasToView(projection, item.snap_end_x, item.snap_end_y);

    const double halfLineWidth = qMax(0.0, item.snap_line_width) / 2.0;
    QRectF bounds(QPointF(qMin(start.x(), end.x()), qMin(start.y(), end.y())),
                  QPointF(qMax(start.x(), end.x()), qMax(start.y(), end.y())));
    bounds =
        bounds.normalized().adjusted(-halfLineWidth, -halfLineWidth, halfLineWidth, halfLineWidth);

    const double markerRadius = item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP
                                    ? qMax(0.0, item.snap_marker_size) * 0.75
                                    : qMax(0.0, item.snap_marker_size) * 0.5;
    const std::uint8_t markerCount = qMin<std::uint8_t>(item.snap_marker_count, 2);
    if (markerCount == 0) {
        bounds = bounds.united(pointBounds(start, markerRadius));
        bounds = bounds.united(pointBounds(end, markerRadius));
    } else {
        const QPointF markers[2] = {
            canvasToView(projection, item.snap_marker0_x, item.snap_marker0_y),
            canvasToView(projection, item.snap_marker1_x, item.snap_marker1_y),
        };
        for (std::uint8_t index = 0; index < markerCount; ++index) {
            bounds = bounds.united(pointBounds(markers[index], markerRadius));
        }
    }

    if (item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP && item.snap_has_label != 0) {
        bounds =
            bounds.united(snapGuideLabelBounds(item.snap_guide_axis, start, end, item.snap_label));
    }

    return bounds;
}

} // namespace

ViewProjection sceneProjection(const SceneDisplayInfo& displayInfo) {
    return ViewProjection{
        displayInfo.camera_center_x, displayInfo.camera_center_y, displayInfo.camera_zoom,
        displayInfo.surface_width,   displayInfo.surface_height,
    };
}

ViewProjection overlayProjection(const OverlayDisplayInfo& displayInfo) {
    return ViewProjection{
        displayInfo.camera_center_x, displayInfo.camera_center_y, displayInfo.camera_zoom,
        displayInfo.surface_width,   displayInfo.surface_height,
    };
}

QPointF canvasToView(double cameraCenterX, double cameraCenterY, double cameraZoom,
                     double surfaceWidth, double surfaceHeight, double x, double y) {
    return canvasToView(
        ViewProjection{
            cameraCenterX,
            cameraCenterY,
            cameraZoom,
            surfaceWidth,
            surfaceHeight,
        },
        x, y);
}

QPointF canvasToView(const ViewProjection& projection, double x, double y) {
    return QPointF(
        (x - projection.cameraCenterX) * projection.cameraZoom + projection.surfaceWidth / 2.0,
        (y - projection.cameraCenterY) * projection.cameraZoom + projection.surfaceHeight / 2.0);
}

QPointF viewToCanvas(double cameraCenterX, double cameraCenterY, double cameraZoom,
                     double surfaceWidth, double surfaceHeight, const QPointF& position) {
    return viewToCanvas(
        ViewProjection{
            cameraCenterX,
            cameraCenterY,
            cameraZoom,
            surfaceWidth,
            surfaceHeight,
        },
        position);
}

QPointF viewToCanvas(const ViewProjection& projection, const QPointF& position) {
    const double zoom = projection.cameraZoom > 0.0 ? projection.cameraZoom : 1.0;
    return QPointF(projection.cameraCenterX + (position.x() - projection.surfaceWidth / 2.0) / zoom,
                   projection.cameraCenterY +
                       (position.y() - projection.surfaceHeight / 2.0) / zoom);
}

QRect alignedRectForBounds(const QRectF& bounds, int paddingPx) {
    if (!bounds.isValid() || bounds.isEmpty()) {
        return {};
    }

    const int padding = qMax(0, paddingPx);
    const int left = static_cast<int>(std::floor(bounds.left())) - padding;
    const int top = static_cast<int>(std::floor(bounds.top())) - padding;
    const int right = static_cast<int>(std::ceil(bounds.right())) + padding;
    const int bottom = static_cast<int>(std::ceil(bounds.bottom())) + padding;
    if (right <= left || bottom <= top) {
        return {};
    }
    return QRect(left, top, right - left, bottom - top);
}

QRectF pointBounds(const QPointF& point, double radius) {
    return QRectF(point.x() - radius, point.y() - radius, radius * 2.0, radius * 2.0);
}

QRectF rotatedRectBounds(const QPointF& center, double width, double height, double rotationRadians,
                         double strokeWidth) {
    const double strokePadding = qMax(0.0, strokeWidth) / 2.0;
    const double halfWidth = qMax(0.0, width) / 2.0;
    const double halfHeight = qMax(0.0, height) / 2.0;
    const double cosTheta = std::abs(std::cos(rotationRadians));
    const double sinTheta = std::abs(std::sin(rotationRadians));
    const double extentX =
        cosTheta * (halfWidth + strokePadding) + sinTheta * (halfHeight + strokePadding);
    const double extentY =
        sinTheta * (halfWidth + strokePadding) + cosTheta * (halfHeight + strokePadding);
    return QRectF(center.x() - extentX, center.y() - extentY, extentX * 2.0, extentY * 2.0);
}

QPointF rotatePoint(const QPointF& point, const QPointF& center, double radians) {
    const double cosTheta = std::cos(radians);
    const double sinTheta = std::sin(radians);
    const double dx = point.x() - center.x();
    const double dy = point.y() - center.y();
    return QPointF(center.x() + dx * cosTheta - dy * sinTheta,
                   center.y() + dx * sinTheta + dy * cosTheta);
}

bool arrowEndpointGeometry(const QVector<QPointF>& points, SnowArrowType arrowType, bool atStart,
                           QPointF* outEndpoint, QPointF* outPreviousPoint, QPointF* outDirection,
                           double* outSegmentLength) {
    if (points.size() < 2) {
        return false;
    }

    if (arrowType == SNOW_ARROW_TYPE_CURVE) {
        const QVector<CurveSegment> segments = curveSegmentsForPoints(points);
        if (segments.isEmpty()) {
            return false;
        }

        const CurveSegment segment = atStart ? segments.first() : segments.last();
        const QPointF endpoint = atStart ? segment.start : segment.end;
        const QPointF previous = pointAtBezier(segment, atStart ? 0.15 : 0.85);
        const QPointF delta(endpoint.x() - previous.x(), endpoint.y() - previous.y());
        const double length = std::hypot(delta.x(), delta.y());
        if (length <= 1e-6) {
            return false;
        }

        if (outEndpoint != nullptr) {
            *outEndpoint = endpoint;
        }
        if (outPreviousPoint != nullptr) {
            *outPreviousPoint = previous;
        }
        if (outDirection != nullptr) {
            *outDirection = QPointF(delta.x() / length, delta.y() / length);
        }
        if (outSegmentLength != nullptr) {
            *outSegmentLength = length;
        }
        return true;
    }

    const QPointF endpoint = atStart ? points.first() : points.last();
    const QPointF previous = atStart ? points[1] : points[points.size() - 2];
    const QPointF delta(endpoint.x() - previous.x(), endpoint.y() - previous.y());
    const double length = std::hypot(delta.x(), delta.y());
    if (length <= 1e-6) {
        return false;
    }

    if (outEndpoint != nullptr) {
        *outEndpoint = endpoint;
    }
    if (outPreviousPoint != nullptr) {
        *outPreviousPoint = previous;
    }
    if (outDirection != nullptr) {
        *outDirection = QPointF(delta.x() / length, delta.y() / length);
    }
    if (outSegmentLength != nullptr) {
        *outSegmentLength = length;
    }
    return true;
}

std::uint32_t clampedArrowPointCount(std::uint32_t count) {
    return qMin<std::uint32_t>(count, SNOW_ARROW_POINT_CAPACITY);
}

std::uint32_t clampedArrowheadPrimitiveCount(std::uint32_t count) {
    return qMin<std::uint32_t>(count, SNOW_ARROWHEAD_PRIMITIVE_CAPACITY);
}

std::uint32_t clampedArrowheadPrimitivePointCount(std::uint32_t count) {
    return qMin<std::uint32_t>(count, SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY);
}

QVector<QPointF> arrowPointsToView(double cameraCenterX, double cameraCenterY, double cameraZoom,
                                   double surfaceWidth, double surfaceHeight,
                                   const SnowArrowPoint* points, std::uint32_t pointCount) {
    return arrowPointsToView(
        ViewProjection{
            cameraCenterX,
            cameraCenterY,
            cameraZoom,
            surfaceWidth,
            surfaceHeight,
        },
        points, pointCount);
}

QVector<QPointF> arrowPointsToView(const ViewProjection& projection, const SnowArrowPoint* points,
                                   std::uint32_t pointCount) {
    QVector<QPointF> out;
    if (points == nullptr) {
        return out;
    }
    out.reserve(static_cast<int>(clampedArrowPointCount(pointCount)));
    for (std::uint32_t index = 0; index < clampedArrowPointCount(pointCount); ++index) {
        out.push_back(canvasToView(projection, points[index].x, points[index].y));
    }
    return out;
}

QRectF arrowheadPrimitiveBounds(const ViewProjection& projection,
                                const SnowArrowheadPrimitive& primitive, double strokeWidth) {
    const double halfStroke = qMax(0.0, strokeWidth) / 2.0;
    switch (primitive.kind) {
    case SNOW_ARROWHEAD_PRIMITIVE_LINE:
    case SNOW_ARROWHEAD_PRIMITIVE_POLYGON: {
        const std::uint32_t pointCount = clampedArrowheadPrimitivePointCount(primitive.point_count);
        if (pointCount == 0) {
            return {};
        }
        QPointF first = canvasToView(projection, primitive.points[0].x, primitive.points[0].y);
        QRectF bounds(first, first);
        bool hasBounds = true;
        for (std::uint32_t index = 1; index < pointCount; ++index) {
            const QPointF point =
                canvasToView(projection, primitive.points[index].x, primitive.points[index].y);
            const QRectF segmentBounds = QRectF(first, point).normalized();
            bounds = hasBounds ? bounds.united(segmentBounds) : segmentBounds;
            hasBounds = true;
            first = point;
        }
        return bounds.normalized().adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
    }
    case SNOW_ARROWHEAD_PRIMITIVE_CIRCLE: {
        const double diameter = primitive.diameter * projection.cameraZoom;
        if (diameter <= 0.0) {
            return {};
        }
        const QPointF center = canvasToView(projection, primitive.center.x, primitive.center.y);
        return QRectF(center.x() - diameter / 2.0, center.y() - diameter / 2.0, diameter, diameter)
            .adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
    }
    case SNOW_ARROWHEAD_PRIMITIVE_NONE:
    default:
        return {};
    }
}

QRectF arrowheadPrimitivesBounds(const ViewProjection& projection,
                                 const SnowArrowheadPrimitive* primitives,
                                 std::uint32_t primitiveCount, double strokeWidth) {
    QRectF bounds;
    bool hasBounds = false;
    if (primitives == nullptr) {
        return bounds;
    }
    for (std::uint32_t index = 0; index < clampedArrowheadPrimitiveCount(primitiveCount); ++index) {
        const QRectF primitiveBounds =
            arrowheadPrimitiveBounds(projection, primitives[index], strokeWidth);
        bounds = hasBounds ? bounds.united(primitiveBounds) : primitiveBounds;
        hasBounds = true;
    }
    return bounds;
}

double arrowheadSize(SnowArrowhead head) {
    switch (head) {
    case SNOW_ARROWHEAD_ARROW:
        return 25.0;
    case SNOW_ARROWHEAD_DIAMOND:
    case SNOW_ARROWHEAD_DIAMOND_OUTLINE:
        return 12.0;
    case SNOW_ARROWHEAD_CROWFOOT_ONE:
    case SNOW_ARROWHEAD_CROWFOOT_MANY:
    case SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY:
        return 20.0;
    case SNOW_ARROWHEAD_NONE:
        return 0.0;
    default:
        return 15.0;
    }
}

double arrowheadAngleDegrees(SnowArrowhead head) {
    switch (head) {
    case SNOW_ARROWHEAD_BAR:
        return 90.0;
    case SNOW_ARROWHEAD_ARROW:
        return 20.0;
    default:
        return 25.0;
    }
}

QPainterPath arrowPathForPoints(const QVector<QPointF>& points, SnowArrowType arrowType,
                                double zoom) {
    constexpr double kElbowArrowCornerRadius = 16.0;
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }
    if (arrowType == SNOW_ARROW_TYPE_ELBOW) {
        return elbowArrowPath(points, kElbowArrowCornerRadius * zoom);
    }
    if (arrowType == SNOW_ARROW_TYPE_CURVE) {
        return curveArrowPath(points);
    }

    path.moveTo(points.first());
    for (int index = 1; index < points.size(); ++index) {
        path.lineTo(points[index]);
    }
    return path;
}

QPainterPath arrowPathFromCommands(const ViewProjection& projection,
                                   const SnowArrowPathCommand* commands,
                                   std::uint32_t commandCount) {
    QPainterPath path;
    if (commands == nullptr || commandCount == 0) {
        return path;
    }

    const std::uint32_t count = commandCount;
    for (std::uint32_t index = 0; index < count; ++index) {
        const SnowArrowPathCommand& command = commands[index];
        const QPointF point = canvasToView(projection, command.point.x, command.point.y);
        switch (command.kind) {
        case SNOW_ARROW_PATH_COMMAND_MOVE_TO:
            path.moveTo(point);
            break;
        case SNOW_ARROW_PATH_COMMAND_LINE_TO:
            path.lineTo(point);
            break;
        case SNOW_ARROW_PATH_COMMAND_QUAD_TO: {
            const QPointF control =
                canvasToView(projection, command.control1.x, command.control1.y);
            path.quadTo(control, point);
            break;
        }
        case SNOW_ARROW_PATH_COMMAND_CUBIC_TO: {
            const QPointF control1 =
                canvasToView(projection, command.control1.x, command.control1.y);
            const QPointF control2 =
                canvasToView(projection, command.control2.x, command.control2.y);
            path.cubicTo(control1, control2, point);
            break;
        }
        case SNOW_ARROW_PATH_COMMAND_NONE:
        default:
            break;
        }
    }
    return path;
}


QRectF sceneItemBounds(const SceneDisplayInfo& displayInfo, const SnowSceneDisplayItem& item) {
    const ViewProjection projection = sceneProjection(displayInfo);
    switch (item.kind) {
    case SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT:
    case SNOW_SCENE_DISPLAY_ITEM_FILTER: {
        const QPointF center = canvasToView(projection, item.center_x, item.center_y);
        return rotatedRectBounds(center, item.width * projection.cameraZoom,
                                 item.height * projection.cameraZoom, item.rotation,
                                 item.stroke_width * projection.cameraZoom);
    }
    case SNOW_SCENE_DISPLAY_ITEM_TEXT:
        return textItemBounds(projection, item);
    case SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER: {
        const QPointF center = canvasToView(projection, item.center_x, item.center_y);
        const double diameter = qMin(item.width, item.height);
        if (!std::isfinite(diameter) || diameter <= 0.0) {
            return {};
        }
        const double resolvedStrokeWidth = item.stroke_width * projection.cameraZoom;
        const double strokeWidth = std::isfinite(resolvedStrokeWidth) ? resolvedStrokeWidth : 0.0;
        return rotatedRectBounds(center, diameter * projection.cameraZoom,
                                 diameter * projection.cameraZoom, item.rotation, strokeWidth);
    }
    case SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR: {
        const QPointF start = canvasToView(projection, item.center_x, item.center_y);
        const QPointF end = canvasToView(projection, item.width, item.height);
        QRectF bounds(start, end);
        bounds = bounds.normalized();
        if (item.arrow_point_count >= 2) {
            const QPointF baselineStart =
                canvasToView(projection, item.arrow_points[0].x, item.arrow_points[0].y);
            const QPointF baselineEnd =
                canvasToView(projection, item.arrow_points[1].x, item.arrow_points[1].y);
            bounds = bounds.united(QRectF(baselineStart, baselineEnd).normalized());
        }
        const double padding = qMax(1.0, item.stroke_width * projection.cameraZoom);
        return bounds.adjusted(-padding, -padding, padding, padding);
    }
    case SNOW_SCENE_DISPLAY_ITEM_ARROW: {
        const QVector<QPointF> points =
            arrowPointsToView(projection, item.arrow_points, item.arrow_point_count);
        QPainterPath path = arrowPathFromCommands(projection, item.arrow_path_commands,
                                                  item.arrow_path_command_count);
        if (path.isEmpty()) {
            path = arrowPathForPoints(points, item.arrow_type, projection.cameraZoom);
        }
        const double strokeWidth = item.stroke_width * projection.cameraZoom;
        QRectF bounds;
        bool hasBounds = false;
        if (!path.isEmpty()) {
            const double halfStroke = qMax(0.0, strokeWidth) / 2.0;
            bounds = path.boundingRect().adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
            hasBounds = true;
        }
        const QRectF primitiveBounds = arrowheadPrimitivesBounds(
            projection, item.arrowhead_primitives, item.arrowhead_primitive_count, strokeWidth);
        if (item.arrowhead_primitive_count > 0) {
            bounds = hasBounds ? bounds.united(primitiveBounds) : primitiveBounds;
        } else if (!path.isEmpty()) {
            const double maxHeadSize =
                qMax(arrowheadSize(item.arrow_start_head), arrowheadSize(item.arrow_end_head)) *
                projection.cameraZoom;
            bounds = bounds.adjusted(-maxHeadSize, -maxHeadSize, maxHeadSize, maxHeadSize);
        }
        return bounds;
    }
    default:
        return {};
    }
}

QRectF sceneItemBounds(const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem& item) {
    QRectF bounds = sceneItemBounds(displayInfo, static_cast<const SnowSceneDisplayItem&>(item));
    if (item.kind != SNOW_SCENE_DISPLAY_ITEM_ARROW || item.pathChunks().empty()) {
        return bounds;
    }

    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    bool hasCanvasBounds = false;
    for (const SnowCanvasSceneItem::PathChunk& chunk : item.pathChunks()) {
        if (hasCanvasBounds) {
            minX = qMin(minX, chunk.canvasBounds.left());
            minY = qMin(minY, chunk.canvasBounds.top());
            maxX = qMax(maxX, chunk.canvasBounds.right());
            maxY = qMax(maxY, chunk.canvasBounds.bottom());
        } else {
            minX = chunk.canvasBounds.left();
            minY = chunk.canvasBounds.top();
            maxX = chunk.canvasBounds.right();
            maxY = chunk.canvasBounds.bottom();
        }
        hasCanvasBounds = true;
    }
    if (!hasCanvasBounds) {
        return bounds;
    }

    const ViewProjection projection = sceneProjection(displayInfo);
    const QPointF topLeft = canvasToView(projection, minX, minY);
    const QPointF bottomRight = canvasToView(projection, maxX, maxY);
    const double halfStroke = qMax(0.0, item.stroke_width * projection.cameraZoom) / 2.0;
    const QRectF pathBounds = QRectF(topLeft, bottomRight)
                                  .normalized()
                                  .adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
    return bounds.isEmpty() ? pathBounds : bounds.united(pathBounds);
}

QRectF overlayItemBounds(const OverlayDisplayInfo& displayInfo,
                         const SnowOverlayDisplayItem& item) {
    const ViewProjection projection = overlayProjection(displayInfo);
    switch (item.kind) {
    case SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT:
        return overlayRectBounds(projection, item);
    case SNOW_OVERLAY_DISPLAY_ITEM_SNAP_GUIDE:
        return snapGuideBounds(projection, item);
    case SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION: {
        const QVector<QPointF> points =
            arrowPointsToView(projection, item.arrow_points, item.arrow_point_count);
        QPainterPath path = arrowPathFromCommands(projection, item.arrow_path_commands,
                                                  item.arrow_path_command_count);
        if (path.isEmpty()) {
            path = arrowPathForPoints(points, item.arrow_type, projection.cameraZoom);
        }
        const double strokeWidth = item.stroke_width * projection.cameraZoom;
        QRectF bounds;
        bool hasBounds = false;
        if (!path.isEmpty()) {
            const double halfStroke = qMax(0.0, strokeWidth) / 2.0;
            bounds = path.boundingRect().adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke);
            hasBounds = true;
        }
        const QRectF primitiveBounds = arrowheadPrimitivesBounds(
            projection, item.arrowhead_primitives, item.arrowhead_primitive_count, strokeWidth);
        if (item.arrowhead_primitive_count > 0) {
            bounds = hasBounds ? bounds.united(primitiveBounds) : primitiveBounds;
        } else if (!path.isEmpty()) {
            const double maxHeadSize =
                qMax(arrowheadSize(item.arrow_start_head), arrowheadSize(item.arrow_end_head)) *
                projection.cameraZoom;
            bounds = bounds.adjusted(-maxHeadSize, -maxHeadSize, maxHeadSize, maxHeadSize);
        }
        return bounds;
    }
    case SNOW_OVERLAY_DISPLAY_ITEM_PEN_FILTER_CONTOUR: {
        const QVector<QPointF> points =
            arrowPointsToView(projection, item.arrow_points, item.arrow_point_count);
        QPainterPath path = arrowPathFromCommands(projection, item.arrow_path_commands,
                                                  item.arrow_path_command_count);
        if (path.isEmpty()) {
            path = arrowPathForPoints(points, SNOW_ARROW_TYPE_STRAIGHT, projection.cameraZoom);
        }
        const double halfStroke = qMax(0.0, item.stroke_width * projection.cameraZoom) / 2.0;
        return path.isEmpty() ? QRectF()
                              : path.boundingRect().adjusted(-halfStroke - 1.0, -halfStroke - 1.0,
                                                             halfStroke + 1.0, halfStroke + 1.0);
    }
    }
    return {};
}

ViewCornerRadii toViewCornerRadii(const SnowCornerRadii& radii, double zoom, const QRectF& rect) {
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
        return {};
    }

    ViewCornerRadii scaled{
        qMax(0.0, radii.top_left * zoom),
        qMax(0.0, radii.top_right * zoom),
        qMax(0.0, radii.bottom_right * zoom),
        qMax(0.0, radii.bottom_left * zoom),
    };

    const double factor =
        std::clamp(std::min({
                       scaleConstraint(rect.width(), scaled.topLeft + scaled.topRight),
                       scaleConstraint(rect.width(), scaled.bottomLeft + scaled.bottomRight),
                       scaleConstraint(rect.height(), scaled.topLeft + scaled.bottomLeft),
                       scaleConstraint(rect.height(), scaled.topRight + scaled.bottomRight),
                   }),
                   0.0, 1.0);

    scaled.topLeft *= factor;
    scaled.topRight *= factor;
    scaled.bottomRight *= factor;
    scaled.bottomLeft *= factor;
    return scaled;
}

QPainterPath roundedRectPath(const QRectF& rect, const ViewCornerRadii& radii) {
    QPainterPath path;
    if (rect.isEmpty()) {
        return path;
    }

    const double left = rect.left();
    const double top = rect.top();
    const double right = rect.right();
    const double bottom = rect.bottom();

    path.moveTo(left + radii.topLeft, top);
    path.lineTo(right - radii.topRight, top);
    if (radii.topRight > 0.0) {
        path.arcTo(
            QRectF(right - radii.topRight * 2.0, top, radii.topRight * 2.0, radii.topRight * 2.0),
            90.0, -90.0);
    } else {
        path.lineTo(right, top);
    }

    path.lineTo(right, bottom - radii.bottomRight);
    if (radii.bottomRight > 0.0) {
        path.arcTo(QRectF(right - radii.bottomRight * 2.0, bottom - radii.bottomRight * 2.0,
                          radii.bottomRight * 2.0, radii.bottomRight * 2.0),
                   0.0, -90.0);
    } else {
        path.lineTo(right, bottom);
    }

    path.lineTo(left + radii.bottomLeft, bottom);
    if (radii.bottomLeft > 0.0) {
        path.arcTo(QRectF(left, bottom - radii.bottomLeft * 2.0, radii.bottomLeft * 2.0,
                          radii.bottomLeft * 2.0),
                   270.0, -90.0);
    } else {
        path.lineTo(left, bottom);
    }

    path.lineTo(left, top + radii.topLeft);
    if (radii.topLeft > 0.0) {
        path.arcTo(QRectF(left, top, radii.topLeft * 2.0, radii.topLeft * 2.0), 180.0, -90.0);
    } else {
        path.lineTo(left, top);
    }
    path.closeSubpath();
    return path;
}

} // namespace snow_canvas_render_geometry
