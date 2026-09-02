#include "snow_canvas_renderer.h"

#include "snow_canvas_display_item.h"
#include "snow_canvas_filter_render.h"
#include "snow_canvas_fill_render.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_render_diagnostics.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_layout.h"
#include "snow_canvas_text_render.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

#include <QBrush>
#include <QFont>
#include <QFontMetricsF>
#include <QFontInfo>
#include <QLineF>
#include <QPainter>
#include <QImage>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QString>
#include <QVector>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace snow_canvas_renderer {
namespace text_layout = snow_canvas_text_layout;
using snow_canvas_render_diagnostics::StageTimer;
namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
thread_local FilterRenderDiagnostics g_filterDiagnostics;

std::uint64_t filterTileHashAppend(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

std::uint64_t filterTileHashDouble(std::uint64_t hash, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return filterTileHashAppend(hash, bits);
}

std::uint64_t filterDependencyFingerprint(const SnowCanvasSceneItem* items,
                                          std::uint32_t end) {
    std::uint64_t hash = 1469598103934665603ULL;
    if (items == nullptr) {
        return hash;
    }
    for (std::uint32_t index = 0; index < end; ++index) {
        const SnowCanvasSceneItem& item = items[index];
        hash = filterTileHashAppend(hash, static_cast<std::uint64_t>(item.kind));
        hash = filterTileHashAppend(hash, item.element_id.index);
        hash = filterTileHashAppend(hash, item.element_id.generation);
        hash = filterTileHashDouble(hash, item.center_x);
        hash = filterTileHashDouble(hash, item.center_y);
        hash = filterTileHashDouble(hash, item.width);
        hash = filterTileHashDouble(hash, item.height);
        hash = filterTileHashDouble(hash, item.rotation);
        hash = filterTileHashDouble(hash, item.opacity);
        hash = filterTileHashAppend(hash, item.penFilterGeometryRevision());
        hash = filterTileHashAppend(hash, item.pathGeometryRevision());
        hash = filterTileHashAppend(hash, item.filter.filter_type);
        hash = filterTileHashDouble(hash, item.filter.strength);
        hash = filterTileHashDouble(hash, item.filter.mosaic_block_size);
        hash = filterTileHashDouble(hash, item.filter.blur_sigma);
        hash = filterTileHashDouble(hash, item.filter.sampling_radius);
    }
    return hash;
}

std::uint64_t filterTileDprBits(qreal dpr) {
    const double value = static_cast<double>(dpr);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct ArrowRenderProjection {
    snow_canvas_render_geometry::ViewProjection view;
    QColor background;
};

using snow_canvas_render_geometry::alignedRectForBounds;
using snow_canvas_render_geometry::arrowEndpointGeometry;
using snow_canvas_render_geometry::arrowheadAngleDegrees;
using snow_canvas_render_geometry::arrowheadSize;
using snow_canvas_render_geometry::arrowPathForPoints;
using snow_canvas_render_geometry::arrowPathFromCommands;
using snow_canvas_render_geometry::arrowPointsToView;
using snow_canvas_render_geometry::canvasToView;
using snow_canvas_render_geometry::overlayItemBounds;
using snow_canvas_render_geometry::rotatePoint;
using snow_canvas_render_geometry::roundedRectPath;
using snow_canvas_render_geometry::sceneItemBounds;
using snow_canvas_render_geometry::toViewCornerRadii;

std::uint64_t filterDependencyFingerprintForRegion(const SnowCanvasSceneItem* items,
                                                   std::uint32_t end,
                                                   const SceneDisplayInfo& displayInfo,
                                                   const QRectF& bounds) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint32_t index = 0; items != nullptr && index < end; ++index) {
        const SnowCanvasSceneItem& item = items[index];
        if (!sceneItemBounds(displayInfo, item).intersects(bounds)) {
            continue;
        }
        hash = filterTileHashAppend(hash, index);
        hash = filterTileHashAppend(hash, static_cast<std::uint64_t>(item.kind));
        hash = filterTileHashAppend(hash, item.element_id.index);
        hash = filterTileHashAppend(hash, item.element_id.generation);
        hash = filterTileHashDouble(hash, item.center_x);
        hash = filterTileHashDouble(hash, item.center_y);
        hash = filterTileHashDouble(hash, item.width);
        hash = filterTileHashDouble(hash, item.height);
        hash = filterTileHashDouble(hash, item.rotation);
        hash = filterTileHashDouble(hash, item.opacity);
        hash = filterTileHashAppend(hash, item.penFilterGeometryRevision());
        hash = filterTileHashAppend(hash, item.pathGeometryRevision());
        hash = filterTileHashAppend(hash, item.filter.filter_type);
        hash = filterTileHashDouble(hash, item.filter.strength);
        hash = filterTileHashDouble(hash, item.filter.mosaic_block_size);
        hash = filterTileHashDouble(hash, item.filter.blur_sigma);
        hash = filterTileHashDouble(hash, item.filter.sampling_radius);
    }
    return hash;
}

void applyMultiSelectionDashStyle(QPen& pen) {
    pen.setStyle(Qt::CustomDashLine);
    pen.setCapStyle(Qt::FlatCap);
    pen.setDashPattern({5.0, 3.0});
}

void applyArrowStrokeStyle(QPen& pen, SnowStrokeStyle style) {
    switch (style) {
    case SNOW_STROKE_STYLE_DASHED:
        pen.setStyle(Qt::DashLine);
        break;
    case SNOW_STROKE_STYLE_DOTTED:
        pen.setStyle(Qt::DotLine);
        break;
    case SNOW_STROKE_STYLE_SOLID:
    default:
        pen.setStyle(Qt::SolidLine);
        break;
    }
}

void applyFreeDrawStrokeStyle(QPen& pen, SnowStrokeStyle style) {
    switch (style) {
    case SNOW_STROKE_STYLE_DASHED:
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({2.0, 2.4});
        break;
    case SNOW_STROKE_STYLE_DOTTED:
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({0.0001, 1.9999});
        break;
    case SNOW_STROKE_STYLE_SOLID:
    default:
        pen.setStyle(Qt::SolidLine);
        break;
    }
}

void applyStrokeStyle(QPen& pen, SnowStrokeStyle style) {
    switch (style) {
    case SNOW_STROKE_STYLE_DASHED:
        pen.setStyle(Qt::DashLine);
        pen.setCapStyle(Qt::RoundCap);
        break;
    case SNOW_STROKE_STYLE_DOTTED:
        pen.setStyle(Qt::DotLine);
        pen.setCapStyle(Qt::RoundCap);
        break;
    case SNOW_STROKE_STYLE_SOLID:
    default:
        pen.setStyle(Qt::SolidLine);
        pen.setCapStyle(Qt::FlatCap);
        break;
    }
}

void applyRectangleStrokeStyle(QPen& pen, SnowStrokeStyle style) {
    switch (style) {
    case SNOW_STROKE_STYLE_DASHED:
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({4.0, 4.0});
        pen.setCapStyle(Qt::RoundCap);
        break;
    case SNOW_STROKE_STYLE_DOTTED:
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({0.01, 3.0});
        pen.setCapStyle(Qt::RoundCap);
        break;
    case SNOW_STROKE_STYLE_SOLID:
    default:
        pen.setStyle(Qt::SolidLine);
        pen.setCapStyle(Qt::FlatCap);
        break;
    }
}

QBrush fillBrushForStyle(const SnowColorRgba8& color, SnowFillStyle style) {
    if (color.a == 0) {
        return Qt::NoBrush;
    }
    QBrush brush(toQColor(color));
    switch (style) {
    case SNOW_FILL_STYLE_LINE:
        brush.setStyle(Qt::BDiagPattern);
        break;
    case SNOW_FILL_STYLE_CROSS_LINE:
        brush.setStyle(Qt::CrossPattern);
        break;
    case SNOW_FILL_STYLE_SOLID:
    default:
        brush.setStyle(Qt::SolidPattern);
        break;
    }
    return brush;
}

void drawSerialNumberText(QPainter& painter, const SnowSceneDisplayItem& item,
                          const QRectF& localRect, double zoom, double strokeWidth) {
    if (item.text_color.a == 0 || item.font_size <= 0.0) {
        return;
    }

    const QString text = QString::number(qMax<std::int64_t>(0, item.serial_number));
    text_layout::SingleLineLayout layout =
        text_layout::createSingleLineLayout(text, painter.font(), item, zoom);
    const QRectF layoutBounds = layout.layoutBounds;
    if (!layoutBounds.isValid() || layoutBounds.isEmpty()) {
        return;
    }
    const QRectF visualBounds = layout.visualBounds;

    const double inset = qMax(0.0, strokeWidth);
    const QRectF contentRect = localRect.adjusted(inset, inset, -inset, -inset);
    const QSizeF contentSize = contentRect.isEmpty() ? localRect.size() : contentRect.size();
    const double resolvedScale = layout.resolution.scale;
    const double widthScale =
        contentSize.width() > 0.0
            ? contentSize.width() / qMax(1.0, visualBounds.width() * resolvedScale)
            : 1.0;
    const double heightScale =
        contentSize.height() > 0.0
            ? contentSize.height() / qMax(1.0, visualBounds.height() * resolvedScale)
            : 1.0;
    const double fitScale = qMin(1.0, qMin(widthScale, heightScale));

    painter.save();
    painter.setPen(toQColor(item.text_color));
    painter.setFont(layout.resolution.font);
    painter.translate(localRect.center());
    painter.scale(resolvedScale * fitScale, resolvedScale * fitScale);
    layout.textLayout().draw(&painter, -visualBounds.center());
    painter.restore();
}

ArrowRenderProjection arrowProjectionForScene(const SceneDisplayInfo& displayInfo) {
    return ArrowRenderProjection{
        snow_canvas_render_geometry::sceneProjection(displayInfo),
        toQColor(displayInfo.clear_color),
    };
}

ArrowRenderProjection focusConnectionProjection(const OverlayDisplayInfo& displayInfo) {
    return ArrowRenderProjection{
        snow_canvas_render_geometry::overlayProjection(displayInfo),
        QColor(Qt::white),
    };
}

void drawArrowhead(QPainter& painter, const QVector<QPointF>& points, SnowArrowType arrowType,
                   bool atStart, SnowArrowhead head, SnowStrokeStyle style, double strokeWidth,
                   double zoom, const QColor& stroke, const QColor& background) {
    if (head == SNOW_ARROWHEAD_NONE || points.size() < 2) {
        return;
    }

    QPointF endpoint;
    QPointF previous;
    QPointF direction;
    double segmentLength = 0.0;
    if (!arrowEndpointGeometry(points, arrowType, atStart, &endpoint, &previous, &direction,
                               &segmentLength)) {
        return;
    }

    const double size = arrowheadSize(head) * zoom;
    const double lengthMultiplier =
        (head == SNOW_ARROWHEAD_DIAMOND || head == SNOW_ARROWHEAD_DIAMOND_OUTLINE) ? 0.25 : 0.5;
    const double minSize = qMin(size, segmentLength * lengthMultiplier);
    const QPointF base(endpoint.x() - direction.x() * minSize,
                       endpoint.y() - direction.y() * minSize);
    const double angle = arrowheadAngleDegrees(head) / kRadiansToDegrees;
    const QPointF wing1 = rotatePoint(base, endpoint, -angle);
    const QPointF wing2 = rotatePoint(base, endpoint, angle);

    painter.save();
    QPen pen(stroke, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    applyArrowStrokeStyle(pen, style);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (head) {
    case SNOW_ARROWHEAD_DOT:
    case SNOW_ARROWHEAD_CIRCLE:
    case SNOW_ARROWHEAD_CIRCLE_OUTLINE: {
        const double diameter = QLineF(base, endpoint).length() + strokeWidth - 2.0;
        const QRectF rect(endpoint.x() - diameter / 2.0, endpoint.y() - diameter / 2.0, diameter,
                          diameter);
        painter.setBrush((head == SNOW_ARROWHEAD_CIRCLE_OUTLINE) ? QBrush(background)
                                                                 : QBrush(stroke));
        painter.drawEllipse(rect);
        break;
    }
    case SNOW_ARROWHEAD_TRIANGLE:
    case SNOW_ARROWHEAD_TRIANGLE_OUTLINE: {
        QPainterPath path;
        path.moveTo(endpoint);
        path.lineTo(wing1);
        path.lineTo(wing2);
        path.closeSubpath();
        painter.setBrush((head == SNOW_ARROWHEAD_TRIANGLE_OUTLINE) ? QBrush(background)
                                                                   : QBrush(stroke));
        painter.drawPath(path);
        break;
    }
    case SNOW_ARROWHEAD_DIAMOND:
    case SNOW_ARROWHEAD_DIAMOND_OUTLINE: {
        const QPointF opposite(endpoint.x() - direction.x() * minSize * 2.0,
                               endpoint.y() - direction.y() * minSize * 2.0);
        QPainterPath path;
        path.moveTo(endpoint);
        path.lineTo(wing1);
        path.lineTo(opposite);
        path.lineTo(wing2);
        path.closeSubpath();
        painter.setBrush((head == SNOW_ARROWHEAD_DIAMOND_OUTLINE) ? QBrush(background)
                                                                  : QBrush(stroke));
        painter.drawPath(path);
        break;
    }
    case SNOW_ARROWHEAD_CROWFOOT_ONE: {
        painter.drawLine(wing1, wing2);
        break;
    }
    case SNOW_ARROWHEAD_CROWFOOT_MANY:
    case SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY: {
        const QPointF crowWing1 = rotatePoint(endpoint, base, -angle);
        const QPointF crowWing2 = rotatePoint(endpoint, base, angle);
        painter.drawLine(crowWing1, base);
        painter.drawLine(crowWing2, base);
        if (head == SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY) {
            painter.drawLine(wing1, wing2);
        }
        break;
    }
    case SNOW_ARROWHEAD_ARROW:
    case SNOW_ARROWHEAD_BAR:
    default:
        painter.drawLine(wing1, endpoint);
        painter.drawLine(wing2, endpoint);
        break;
    }

    painter.restore();
}

void applyArrowheadDashMode(QPen& pen, SnowArrowheadDashMode dashMode,
                            SnowStrokeStyle inheritedStyle) {
    switch (dashMode) {
    case SNOW_ARROWHEAD_DASH_SOLID:
        pen.setStyle(Qt::SolidLine);
        break;
    case SNOW_ARROWHEAD_DASH_DOTTED_CAP:
        pen.setStyle(Qt::DotLine);
        break;
    case SNOW_ARROWHEAD_DASH_INHERIT:
    default:
        applyArrowStrokeStyle(pen, inheritedStyle);
        break;
    }
}

QPointF arrowheadPointToView(const snow_canvas_render_geometry::ViewProjection& projection,
                             const SnowArrowPoint& point) {
    return canvasToView(projection, point.x, point.y);
}

void drawArrowheadPrimitive(QPainter& painter, const ArrowRenderProjection& projection,
                            const SnowArrowheadPrimitive& primitive,
                            SnowStrokeStyle inheritedStyle, const QColor& stroke,
                            double strokeWidth) {
    if (!stroke.isValid() || stroke.alpha() == 0 || strokeWidth <= 0.0) {
        return;
    }

    QPen pen(stroke, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    applyArrowheadDashMode(pen, primitive.dash_mode, inheritedStyle);

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (primitive.kind) {
    case SNOW_ARROWHEAD_PRIMITIVE_LINE: {
        if (primitive.point_count < 2) {
            break;
        }
        painter.drawLine(arrowheadPointToView(projection.view, primitive.points[0]),
                         arrowheadPointToView(projection.view, primitive.points[1]));
        break;
    }
    case SNOW_ARROWHEAD_PRIMITIVE_POLYGON: {
        const std::uint32_t pointCount =
            qMin<std::uint32_t>(primitive.point_count, SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY);
        if (pointCount < 2) {
            break;
        }
        QPainterPath path;
        path.moveTo(arrowheadPointToView(projection.view, primitive.points[0]));
        for (std::uint32_t index = 1; index < pointCount; ++index) {
            path.lineTo(arrowheadPointToView(projection.view, primitive.points[index]));
        }
        painter.setBrush(primitive.fill_mode == SNOW_ARROWHEAD_FILL_BACKGROUND
                             ? QBrush(projection.background)
                             : QBrush(stroke));
        painter.drawPath(path);
        break;
    }
    case SNOW_ARROWHEAD_PRIMITIVE_CIRCLE: {
        const double diameter = primitive.diameter * projection.view.cameraZoom;
        if (diameter <= 0.0) {
            break;
        }
        const QPointF center = arrowheadPointToView(projection.view, primitive.center);
        painter.setBrush(primitive.fill_mode == SNOW_ARROWHEAD_FILL_BACKGROUND
                             ? QBrush(projection.background)
                             : QBrush(stroke));
        painter.drawEllipse(
            QRectF(center.x() - diameter / 2.0, center.y() - diameter / 2.0, diameter, diameter));
        break;
    }
    case SNOW_ARROWHEAD_PRIMITIVE_NONE:
    default:
        break;
    }

    painter.restore();
}

void drawArrowheadPrimitives(QPainter& painter, const ArrowRenderProjection& projection,
                             const SnowArrowheadPrimitive* primitives, std::uint32_t primitiveCount,
                             SnowStrokeStyle inheritedStyle, const QColor& stroke,
                             double strokeWidth) {
    if (primitives == nullptr) {
        return;
    }
    const std::uint32_t count =
        qMin<std::uint32_t>(primitiveCount, SNOW_ARROWHEAD_PRIMITIVE_CAPACITY);
    for (std::uint32_t index = 0; index < count; ++index) {
        drawArrowheadPrimitive(painter, projection, primitives[index], inheritedStyle, stroke,
                               strokeWidth);
    }
}

void drawArrowPath(QPainter& painter, const QVector<QPointF>& points,
                   const QPainterPath* pathOverride, const ArrowRenderProjection& projection,
                   const SnowArrowheadPrimitive* arrowheadPrimitives,
                   std::uint32_t arrowheadPrimitiveCount, SnowArrowType arrowType,
                   SnowArrowhead startHead, SnowArrowhead endHead, SnowStrokeStyle style,
                   bool isFreeDraw, bool roundCaps, const QColor& stroke, double strokeWidth,
                   double zoom, const QColor& background) {
    if (points.size() < 2 || !stroke.isValid() || stroke.alpha() == 0 || strokeWidth <= 0.0) {
        return;
    }

    painter.save();
    QPen pen(stroke, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (isFreeDraw) {
        applyFreeDrawStrokeStyle(pen, style);
    } else {
        applyArrowStrokeStyle(pen, style);
    }
    if (roundCaps) {
        pen.setCapStyle(Qt::RoundCap);
    }
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(pathOverride != nullptr && !pathOverride->isEmpty()
                         ? *pathOverride
                         : arrowPathForPoints(points, arrowType, zoom));
    if (arrowheadPrimitiveCount > 0) {
        drawArrowheadPrimitives(painter, projection, arrowheadPrimitives, arrowheadPrimitiveCount,
                                style, stroke, strokeWidth);
    } else {
        drawArrowhead(painter, points, arrowType, true, startHead, style, strokeWidth, zoom, stroke,
                      background);
        drawArrowhead(painter, points, arrowType, false, endHead, style, strokeWidth, zoom, stroke,
                      background);
    }
    painter.restore();
}

void drawArrowDisplayItem(QPainter& painter, const ArrowRenderProjection& projection,
                          const SnowArrowPoint* points, std::uint32_t pointCount,
                          SnowArrowType arrowType, SnowArrowhead startHead, SnowArrowhead endHead,
                          SnowStrokeStyle strokeStyle, bool isFreeDraw, bool roundCaps,
                          const SnowColorRgba8& stroke, double strokeWidth,
                          const QPainterPath* pathOverride,
                          const SnowArrowheadPrimitive* arrowheadPrimitives,
                          std::uint32_t arrowheadPrimitiveCount) {
    const QVector<QPointF> viewPoints = arrowPointsToView(projection.view, points, pointCount);
    drawArrowPath(painter, viewPoints, pathOverride, projection, arrowheadPrimitives,
                  arrowheadPrimitiveCount, arrowType, startHead, endHead, strokeStyle, isFreeDraw,
                  roundCaps, toQColor(stroke), strokeWidth * projection.view.cameraZoom,
                  projection.view.cameraZoom, projection.background);
}

void drawRectItem(QPainter& painter, const SceneDisplayInfo& displayInfo,
                  const SnowCanvasSceneItem& item) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::sceneProjection(displayInfo);
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    const double zoom = projection.cameraZoom;
    if (!std::isfinite(item.width) || !std::isfinite(item.height) || !std::isfinite(zoom) ||
        item.width <= 0.0 || item.height <= 0.0 || zoom <= 0.0) {
        return;
    }
    const bool hasFill = item.fill.a != 0;
    const bool hasStroke = item.stroke.a != 0 && item.stroke_width > 0.0;
    if (!hasFill && !hasStroke) {
        return;
    }

    if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_RECTANGLE && std::abs(item.rotation) <= 1e-12 &&
        !item.hasRoundedCorners() && item.fill_style == SNOW_FILL_STYLE_SOLID) {
        const QRectF viewRect(center.x() - item.width * zoom / 2.0,
                              center.y() - item.height * zoom / 2.0, item.width * zoom,
                              item.height * zoom);
        painter.save();
        if (item.blend_mode == SNOW_BLEND_MODE_MULTIPLY) {
            painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        }
        painter.setOpacity(qBound(0.0, item.opacity, 1.0));
        if (!hasStroke) {
            painter.fillRect(viewRect, toQColor(item.fill));
        } else {
            QPen viewPen(toQColor(item.stroke), item.stroke_width * zoom);
            applyRectangleStrokeStyle(viewPen, item.stroke_style);
            viewPen.setJoinStyle(Qt::MiterJoin);
            painter.setPen(viewPen);
            painter.setBrush(hasFill ? QBrush(toQColor(item.fill)) : QBrush(Qt::NoBrush));
            painter.drawRect(viewRect);
        }
        painter.restore();
        return;
    }

    painter.save();
    if (item.blend_mode == SNOW_BLEND_MODE_MULTIPLY) {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    }
    painter.translate(center);
    painter.rotate(item.rotation * kRadiansToDegrees);
    painter.scale(zoom, zoom);
    painter.setOpacity(qBound(0.0, item.opacity, 1.0));

    QPen pen;
    if (!hasStroke) {
        pen.setStyle(Qt::NoPen);
    } else {
        pen = QPen(toQColor(item.stroke), item.stroke_width);
        applyRectangleStrokeStyle(pen, item.stroke_style);
        pen.setJoinStyle(Qt::MiterJoin);
    }

    const QRectF& localRect = item.localRect();
    const auto radii = toViewCornerRadii(item.corner_radii, 1.0, localRect);
    QPainterPath fillPath;
    if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
        fillPath.addEllipse(localRect);
    } else if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
        fillPath.moveTo(localRect.center().x(), localRect.top());
        fillPath.lineTo(localRect.right(), localRect.center().y());
        fillPath.lineTo(localRect.center().x(), localRect.bottom());
        fillPath.lineTo(localRect.left(), localRect.center().y());
        fillPath.closeSubpath();
    } else {
        fillPath = roundedRectPath(localRect, radii);
    }
    const auto drawShape = [&]() {
        if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
            painter.drawEllipse(localRect);
        } else if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
            painter.drawPath(fillPath);
        } else if (!item.hasRoundedCorners()) {
            painter.drawRect(localRect);
        } else if (item.hasUniformCorners()) {
            painter.drawRoundedRect(localRect, radii.topLeft, radii.topLeft);
        } else {
            painter.drawPath(item.localRectanglePath());
        }
    };

    if (item.fill_style == SNOW_FILL_STYLE_SOLID) {
        painter.setPen(pen);
        painter.setBrush(hasFill ? QBrush(toQColor(item.fill)) : QBrush(Qt::NoBrush));
        drawShape();
    } else {
        if (hasFill) {
            painter.setPen(Qt::NoPen);
            snow_canvas_fill_render::drawStyledFill(painter, fillPath, item.fill, item.fill_style,
                                                    item.stroke_width);
        }
        if (hasStroke) {
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            drawShape();
        }
    }
    painter.restore();
}

QPainterPath projectedArrowPath(const ArrowRenderProjection& projection,
                                const SnowSceneDisplayItem& item) {
    if (item.arrow_path_commands == nullptr || item.arrow_path_command_count == 0) {
        return {};
    }
    return arrowPathFromCommands(projection.view, item.arrow_path_commands,
                                 item.arrow_path_command_count);
}

QTransform canvasToViewTransform(const snow_canvas_render_geometry::ViewProjection& view) {
    QTransform transform;
    transform.translate(view.surfaceWidth * 0.5, view.surfaceHeight * 0.5);
    transform.scale(view.cameraZoom, view.cameraZoom);
    transform.translate(-view.cameraCenterX, -view.cameraCenterY);
    return transform;
}

QRectF viewRectToCanvas(const snow_canvas_render_geometry::ViewProjection& view,
                        const QRectF& rect) {
    const QPointF topLeft = snow_canvas_render_geometry::viewToCanvas(view, rect.topLeft());
    const QPointF bottomRight = snow_canvas_render_geometry::viewToCanvas(view, rect.bottomRight());
    return QRectF(topLeft, bottomRight).normalized();
}

void drawOwnedPathChunks(QPainter& painter, const ArrowRenderProjection& projection,
                         const SnowCanvasSceneItem& item) {
    const double strokeWidth = item.stroke_width;
    const QColor stroke = toQColor(item.stroke);
    if (item.pathChunks().empty()) {
        return;
    }
    painter.save();
    const bool hasExplicitClip = painter.hasClipping();
    const QRectF canvasVisible =
        hasExplicitClip ? viewRectToCanvas(projection.view, painter.clipBoundingRect())
                              .adjusted(-strokeWidth, -strokeWidth, strokeWidth, strokeWidth)
                        : QRectF();
    painter.setWorldTransform(canvasToViewTransform(projection.view), true);

    if (item.pathIsClosed() && item.fill.a != 0 && !item.aggregateClosedPath().isEmpty()) {
        snow_canvas_fill_render::drawStyledFill(painter, item.aggregateClosedPath(), item.fill,
                                                item.fill_style, item.stroke_width);
    }

    if (stroke.alpha() != 0 && strokeWidth > 0.0) {
        std::vector<std::uint32_t> visibleChunks;
        if (hasExplicitClip) {
            item.queryPathChunks(canvasVisible, &visibleChunks);
        } else {
            visibleChunks.resize(item.pathChunks().size());
            std::iota(visibleChunks.begin(), visibleChunks.end(), 0u);
        }
        for (std::uint32_t index : visibleChunks) {
            if (index >= item.pathChunks().size()) {
                continue;
            }
            const SnowCanvasSceneItem::PathChunk& chunk = item.pathChunks()[index];
            QPen pen(stroke, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            if (item.is_free_draw != 0) {
                applyFreeDrawStrokeStyle(pen, item.arrow_stroke_style);
            } else {
                applyArrowStrokeStyle(pen, item.arrow_stroke_style);
            }
            if (pen.style() == Qt::CustomDashLine || pen.style() == Qt::DashLine ||
                pen.style() == Qt::DotLine || pen.style() == Qt::DashDotLine ||
                pen.style() == Qt::DashDotDotLine) {
                pen.setDashOffset(-chunk.cumulativeStartLength / strokeWidth);
            }
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(chunk.canvasPath);
        }
    }
    painter.restore();
}

void drawArrowItem(QPainter& painter, const SceneDisplayInfo& displayInfo,
                   const SnowCanvasSceneItem& item) {
    painter.save();
    if (item.blend_mode == SNOW_BLEND_MODE_MULTIPLY) {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    }
    painter.setOpacity(qBound(0.0, item.opacity, 1.0));
    const ArrowRenderProjection projection = arrowProjectionForScene(displayInfo);
    if (!item.pathChunks().empty()) {
        drawOwnedPathChunks(painter, projection, item);
        const QVector<QPointF> viewPoints =
            arrowPointsToView(projection.view, item.arrow_points, item.arrow_point_count);
        const double viewStrokeWidth = item.stroke_width * projection.view.cameraZoom;
        if (item.arrowhead_primitive_count > 0) {
            drawArrowheadPrimitives(painter, projection, item.arrowhead_primitives,
                                    item.arrowhead_primitive_count, item.arrow_stroke_style,
                                    toQColor(item.stroke), viewStrokeWidth);
        } else if (viewPoints.size() >= 2) {
            drawArrowhead(painter, viewPoints, item.arrow_type, true, item.arrow_start_head,
                          item.arrow_stroke_style, viewStrokeWidth, projection.view.cameraZoom,
                          toQColor(item.stroke), projection.background);
            drawArrowhead(painter, viewPoints, item.arrow_type, false, item.arrow_end_head,
                          item.arrow_stroke_style, viewStrokeWidth, projection.view.cameraZoom,
                          toQColor(item.stroke), projection.background);
        }
    } else {
        QPainterPath rustPath = projectedArrowPath(projection, item);
        const bool fallbackClosed =
            item.arrow_point_count >= 3 && item.arrow_points != nullptr &&
            item.arrow_points[0].x == item.arrow_points[item.arrow_point_count - 1].x &&
            item.arrow_points[0].y == item.arrow_points[item.arrow_point_count - 1].y;
        if (fallbackClosed && item.fill.a != 0 && !rustPath.isEmpty()) {
            QPainterPath fillPath = rustPath;
            fillPath.closeSubpath();
            snow_canvas_fill_render::drawStyledFill(painter, fillPath, item.fill, item.fill_style,
                                                    item.stroke_width * projection.view.cameraZoom);
        }
        drawArrowDisplayItem(painter, projection, item.arrow_points, item.arrow_point_count,
                             item.arrow_type, item.arrow_start_head, item.arrow_end_head,
                             item.arrow_stroke_style, item.is_free_draw != 0,
                             item.blend_mode == SNOW_BLEND_MODE_MULTIPLY, item.stroke,
                             item.stroke_width, rustPath.isEmpty() ? nullptr : &rustPath,
                             item.arrowhead_primitives, item.arrowhead_primitive_count);
    }
    painter.restore();
}

void drawTextItem(QPainter& painter, const SceneDisplayInfo& displayInfo,
                  const SnowSceneDisplayItem& item, bool drawContents = true) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::sceneProjection(displayInfo);
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    const double zoom = projection.cameraZoom;
    const double width = item.width * zoom;
    const double height = item.height * zoom;
    if (width <= 0.0 || height <= 0.0) {
        return;
    }

    painter.save();
    painter.translate(center);
    painter.rotate(item.rotation * kRadiansToDegrees);
    painter.setOpacity(qBound(0.0, item.opacity, 1.0));

    const QRectF localRect(-width / 2.0, -height / 2.0, width, height);
    if (item.fill.a != 0) {
        snow_canvas_text_render::drawBackground(painter, item, painter.font(), localRect, zoom);
    }

    snow_canvas_text_render::drawStroke(painter, item, painter.font(), localRect, zoom);
    if (drawContents) {
        snow_canvas_text_render::drawContents(painter, item, painter.font(), localRect, zoom);
    }
    painter.restore();
}

void drawSerialNumberItem(QPainter& painter, const SceneDisplayInfo& displayInfo,
                          const SnowSceneDisplayItem& item) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::sceneProjection(displayInfo);
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    const double zoom = projection.cameraZoom;
    const double diameter = qMin(item.width, item.height) * zoom;
    const double resolvedStrokeWidth = item.stroke_width * zoom;
    const double strokeWidth = std::isfinite(resolvedStrokeWidth) ? resolvedStrokeWidth : 0.0;
    if (!std::isfinite(diameter) || diameter <= 0.0) {
        return;
    }
    if ((item.fill.a == 0) && (item.stroke.a == 0 || strokeWidth <= 0.0)) {
        return;
    }

    painter.save();
    painter.translate(center);
    painter.rotate(item.rotation * kRadiansToDegrees);
    painter.setOpacity(qBound(0.0, item.opacity, 1.0));

    const QRectF localRect(-diameter / 2.0, -diameter / 2.0, diameter, diameter);
    QPen pen;
    if (item.stroke.a == 0 || strokeWidth <= 0.0) {
        pen.setStyle(Qt::NoPen);
    } else {
        pen = QPen(toQColor(item.stroke), strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        applyStrokeStyle(pen, item.stroke_style);
    }
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPainterPath backgroundPath;
    backgroundPath.addEllipse(localRect);
    snow_canvas_fill_render::drawTextBackgroundFill(painter, backgroundPath, item.fill,
                                                    item.fill_style, item.font_size, zoom);
    painter.drawEllipse(localRect);

    drawSerialNumberText(painter, item, localRect, zoom, strokeWidth);

    painter.restore();
}

void drawSerialNumberConnectorItem(QPainter& painter, const SceneDisplayInfo& displayInfo,
                                   const SnowSceneDisplayItem& item) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::sceneProjection(displayInfo);
    const double strokeWidth = item.stroke_width * projection.cameraZoom;
    if (item.stroke.a == 0 || strokeWidth <= 0.0) {
        return;
    }

    const QPointF start = canvasToView(projection, item.center_x, item.center_y);
    const QPointF end = canvasToView(projection, item.width, item.height);

    painter.save();
    painter.setOpacity(qBound(0.0, item.opacity, 1.0));
    painter.setPen(
        QPen(toQColor(item.stroke), strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (item.arrow_point_count >= 2) {
        const QPointF baselineStart =
            canvasToView(projection, item.arrow_points[0].x, item.arrow_points[0].y);
        const QPointF baselineEnd =
            canvasToView(projection, item.arrow_points[1].x, item.arrow_points[1].y);
        painter.drawLine(baselineStart, baselineEnd);
    }
    painter.drawLine(start, end);
    painter.restore();
}

void drawSceneItem(QPainter& painter, const SceneDisplayInfo& displayInfo,
                   const SnowCanvasSceneItem& item) {
    switch (item.kind) {
    case SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT:
        drawRectItem(painter, displayInfo, item);
        break;
    case SNOW_SCENE_DISPLAY_ITEM_ARROW:
        drawArrowItem(painter, displayInfo, item);
        break;
    case SNOW_SCENE_DISPLAY_ITEM_TEXT:
        drawTextItem(painter, displayInfo, item);
        break;
    case SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER:
        drawSerialNumberItem(painter, displayInfo, item);
        break;
    case SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR:
        drawSerialNumberConnectorItem(painter, displayInfo, item);
        break;
    case SNOW_SCENE_DISPLAY_ITEM_FILTER:
    default:
        break;
    }
}

void drawOverlayRectItem(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                         const SnowOverlayDisplayItem& item) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::overlayProjection(displayInfo);
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    const double zoom = projection.cameraZoom;
    const double width = item.width * zoom;
    const double height = item.height * zoom;
    const double strokeWidth = item.stroke_width * zoom;

    if (item.rect_kind == SNOW_OVERLAY_RECT_TEXT_HOVER_UNDERLINE) {
        if (item.stroke.a == 0 || width <= 0.0 || strokeWidth <= 0.0) {
            return;
        }
        painter.save();
        painter.translate(center);
        painter.rotate(item.rotation * kRadiansToDegrees);
        painter.setPen(
            QPen(toQColor(item.stroke), strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(-width / 2.0, height / 2.0), QPointF(width / 2.0, height / 2.0));
        painter.restore();
        return;
    }

    painter.save();
    painter.translate(center);
    painter.rotate(item.rotation * kRadiansToDegrees);

    QPen pen;
    if (item.stroke.a == 0 || strokeWidth <= 0.0) {
        pen.setStyle(Qt::NoPen);
    } else {
        pen = QPen(toQColor(item.stroke), strokeWidth);
        if (item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME ||
            item.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME) {
            applyMultiSelectionDashStyle(pen);
        }
        pen.setJoinStyle(Qt::MiterJoin);
    }
    painter.setPen(pen);

    const QRectF localRect(-width / 2.0, -height / 2.0, width, height);
    const bool isArrowHandle = item.rect_kind == SNOW_OVERLAY_RECT_ARROW_ENDPOINT_HANDLE ||
                               item.rect_kind == SNOW_OVERLAY_RECT_ARROW_FOCUS_HANDLE ||
                               item.rect_kind == SNOW_OVERLAY_RECT_ARROW_SEGMENT_HANDLE;
    const bool isEraserCursor = item.rect_kind == SNOW_OVERLAY_RECT_ERASER_CURSOR;
    if (isArrowHandle || isEraserCursor) {
        painter.setBrush(fillBrushForStyle(item.fill, item.fill_style));
        painter.drawEllipse(localRect);
    } else {
        const QPainterPath path =
            roundedRectPath(localRect, toViewCornerRadii(item.corner_radii, zoom, localRect));
        snow_canvas_fill_render::drawStyledFill(painter, path, item.fill, item.fill_style,
                                                item.stroke_width, zoom);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
    painter.restore();
}

void drawOverlayRectItem(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                         const SnowCanvasOverlayItem& item) {
    const auto projection = snow_canvas_render_geometry::overlayProjection(displayInfo);
    const double zoom = projection.cameraZoom;
    if (item.rect_kind == SNOW_OVERLAY_RECT_TEXT_HOVER_UNDERLINE) {
        const double width = item.width * zoom;
        const double height = item.height * zoom;
        const double strokeWidth = item.stroke_width * zoom;
        if (!std::isfinite(width) || !std::isfinite(strokeWidth) || !(zoom > 0.0) ||
            item.stroke.a == 0 || width <= 0.0 || strokeWidth <= 0.0) {
            return;
        }
        painter.save();
        painter.translate(canvasToView(projection, item.center_x, item.center_y));
        painter.rotate(item.rotation * kRadiansToDegrees);
        painter.setPen(
            QPen(toQColor(item.stroke), strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(-width / 2.0, height / 2.0), QPointF(width / 2.0, height / 2.0));
        painter.restore();
        return;
    }
    if (!std::isfinite(item.width) || !std::isfinite(item.height) || !(zoom > 0.0) ||
        item.width <= 0.0 || item.height <= 0.0) {
        return;
    }
    const bool hasFill = item.fill.a != 0;
    const bool hasStroke = item.stroke.a != 0 && item.stroke_width > 0.0;
    if (!hasFill && !hasStroke) {
        return;
    }

    const bool isArrowHandle = item.rect_kind == SNOW_OVERLAY_RECT_ARROW_ENDPOINT_HANDLE ||
                               item.rect_kind == SNOW_OVERLAY_RECT_ARROW_FOCUS_HANDLE ||
                               item.rect_kind == SNOW_OVERLAY_RECT_ARROW_SEGMENT_HANDLE;
    const bool isEraserCursor = item.rect_kind == SNOW_OVERLAY_RECT_ERASER_CURSOR;
    if (std::abs(item.rotation) <= 1e-12 && !item.hasRoundedCorners() && !isArrowHandle &&
        !isEraserCursor && item.fill_style == SNOW_FILL_STYLE_SOLID) {
        const QPointF center = canvasToView(projection, item.center_x, item.center_y);
        const QRectF viewRect(center.x() - item.width * zoom / 2.0,
                              center.y() - item.height * zoom / 2.0, item.width * zoom,
                              item.height * zoom);
        painter.save();
        if (!hasStroke) {
            painter.fillRect(viewRect, toQColor(item.fill));
        } else {
            QPen viewPen(toQColor(item.stroke), item.stroke_width * zoom);
            if (item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME ||
                item.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME) {
                applyMultiSelectionDashStyle(viewPen);
            }
            viewPen.setJoinStyle(Qt::MiterJoin);
            painter.setPen(viewPen);
            painter.setBrush(hasFill ? QBrush(toQColor(item.fill)) : QBrush(Qt::NoBrush));
            painter.drawRect(viewRect);
        }
        painter.restore();
        return;
    }

    painter.save();
    painter.translate(canvasToView(projection, item.center_x, item.center_y));
    painter.rotate(item.rotation * kRadiansToDegrees);
    painter.scale(zoom, zoom);

    QPen pen;
    if (hasStroke) {
        pen = QPen(toQColor(item.stroke), item.stroke_width);
        if (item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME ||
            item.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME) {
            applyMultiSelectionDashStyle(pen);
        }
        pen.setJoinStyle(Qt::MiterJoin);
    } else {
        pen.setStyle(Qt::NoPen);
    }

    const QRectF& localRect = item.localRect();
    const auto radii = toViewCornerRadii(item.corner_radii, 1.0, localRect);
    const QPainterPath fillPath = roundedRectPath(localRect, radii);
    const auto drawShape = [&]() {
        if (isArrowHandle || isEraserCursor) {
            painter.drawEllipse(localRect);
        } else if (!item.hasRoundedCorners()) {
            painter.drawRect(localRect);
        } else if (item.hasUniformCorners()) {
            painter.drawRoundedRect(localRect, radii.topLeft, radii.topLeft);
        } else {
            painter.drawPath(item.localRectanglePath());
        }
    };

    if (item.fill_style == SNOW_FILL_STYLE_SOLID || isArrowHandle) {
        painter.setPen(pen);
        painter.setBrush(hasFill ? QBrush(toQColor(item.fill)) : QBrush(Qt::NoBrush));
        drawShape();
    } else {
        if (hasFill) {
            painter.setPen(Qt::NoPen);
            snow_canvas_fill_render::drawStyledFill(painter, fillPath, item.fill, item.fill_style,
                                                    item.stroke_width);
        }
        if (hasStroke) {
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            drawShape();
        }
    }
    painter.restore();
}

void drawFocusConnectionItem(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                             const SnowOverlayDisplayItem& item) {
    const ArrowRenderProjection projection = focusConnectionProjection(displayInfo);
    QPainterPath rustPath = arrowPathFromCommands(projection.view, item.arrow_path_commands,
                                                  item.arrow_path_command_count);
    drawArrowDisplayItem(painter, projection, item.arrow_points, item.arrow_point_count,
                         item.arrow_type, item.arrow_start_head, item.arrow_end_head,
                         item.arrow_stroke_style, false, false, item.stroke, item.stroke_width,
                         rustPath.isEmpty() ? nullptr : &rustPath, item.arrowhead_primitives,
                         item.arrowhead_primitive_count);
}

void drawPenFilterContourItem(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                              const SnowOverlayDisplayItem& item) {
    const ArrowRenderProjection projection = focusConnectionProjection(displayInfo);
    const double strokeWidth = item.stroke_width * projection.view.cameraZoom;
    if (!std::isfinite(strokeWidth) || strokeWidth <= 0.0 || item.stroke.a == 0) {
        return;
    }

    const QVector<QPointF> points =
        arrowPointsToView(projection.view, item.arrow_points, item.arrow_point_count);
    QPainterPath centerline = arrowPathFromCommands(projection.view, item.arrow_path_commands,
                                                    item.arrow_path_command_count);
    if (centerline.isEmpty()) {
        centerline =
            arrowPathForPoints(points, SNOW_ARROW_TYPE_STRAIGHT, projection.view.cameraZoom);
    }
    if (centerline.isEmpty()) {
        return;
    }

    QPainterPathStroker stroker;
    stroker.setWidth(strokeWidth);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    const QPainterPath contour = stroker.createStroke(centerline).simplified();

    painter.save();
    painter.setPen(QPen(toQColor(item.stroke), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(contour);
    painter.restore();
}

void drawCrossMarker(QPainter& painter, const QPointF& point, double size) {
    if (size <= 0.0) {
        return;
    }

    const double half = size * 0.35;
    painter.drawLine(QPointF(point.x() - half, point.y() - half),
                     QPointF(point.x() + half, point.y() + half));
    painter.drawLine(QPointF(point.x() - half, point.y() + half),
                     QPointF(point.x() + half, point.y() - half));
}

void drawTickMarker(QPainter& painter, const QPointF& point, SnowSnapGuideAxis axis, double size) {
    if (size <= 0.0) {
        return;
    }

    const double half = size / 2.0;
    if (axis == SNOW_SNAP_GUIDE_HORIZONTAL) {
        painter.drawLine(QPointF(point.x(), point.y() - half),
                         QPointF(point.x(), point.y() + half));
    } else {
        painter.drawLine(QPointF(point.x() - half, point.y()),
                         QPointF(point.x() + half, point.y()));
    }
}

void drawGapLabel(QPainter& painter, SnowSnapGuideAxis axis, const QPointF& start,
                  const QPointF& end, double label, const QColor& color) {
    const QString text = QString::number(label, 'f', 0);
    QFont labelFont = painter.font();
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

    painter.save();
    painter.setFont(labelFont);
    painter.setPen(color);
    painter.drawText(textOrigin, text);
    painter.restore();
}

void drawSnapGuide(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                   const SnowOverlayDisplayItem& item) {
    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::overlayProjection(displayInfo);
    const QPointF start = canvasToView(projection, item.snap_start_x, item.snap_start_y);
    const QPointF end = canvasToView(projection, item.snap_end_x, item.snap_end_y);

    painter.save();

    QPen pen(toQColor(item.snap_color), item.snap_line_width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    if (item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP && item.snap_gap_dash_length > 0.0 &&
        item.snap_gap_dash_gap > 0.0) {
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({static_cast<qreal>(item.snap_gap_dash_length),
                            static_cast<qreal>(item.snap_gap_dash_gap)});
    }
    painter.setPen(pen);
    painter.drawLine(start, end);

    const double markerSize = item.snap_marker_size;
    const std::uint8_t markerCount = qMin<std::uint8_t>(item.snap_marker_count, 2);
    QPointF markers[2] = {
        canvasToView(projection, item.snap_marker0_x, item.snap_marker0_y),
        canvasToView(projection, item.snap_marker1_x, item.snap_marker1_y),
    };

    if (markerCount == 0) {
        if (item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP) {
            drawTickMarker(painter, start, item.snap_guide_axis, markerSize * 1.5);
            drawTickMarker(painter, end, item.snap_guide_axis, markerSize * 1.5);
        } else {
            drawCrossMarker(painter, start, markerSize);
            drawCrossMarker(painter, end, markerSize);
        }
    } else {
        for (std::uint8_t index = 0; index < markerCount; ++index) {
            if (item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP) {
                drawTickMarker(painter, markers[index], item.snap_guide_axis, markerSize * 1.5);
            } else {
                drawCrossMarker(painter, markers[index], markerSize);
            }
        }
    }

    if (item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP && item.snap_has_label != 0) {
        drawGapLabel(painter, item.snap_guide_axis, start, end, item.snap_label,
                     toQColor(item.snap_color));
    }

    painter.restore();
}

void drawOverlayItem(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                     const SnowOverlayDisplayItem& item) {
    switch (item.kind) {
    case SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT:
        drawOverlayRectItem(painter, displayInfo, item);
        break;
    case SNOW_OVERLAY_DISPLAY_ITEM_SNAP_GUIDE:
        drawSnapGuide(painter, displayInfo, item);
        break;
    case SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION:
        drawFocusConnectionItem(painter, displayInfo, item);
        break;
    case SNOW_OVERLAY_DISPLAY_ITEM_PEN_FILTER_CONTOUR:
        drawPenFilterContourItem(painter, displayInfo, item);
        break;
    }
}

QPainterPath filterClipPath(const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem& item) {
    const auto projection = snow_canvas_render_geometry::sceneProjection(displayInfo);
    if (item.is_free_draw != 0 && item.arrow_points != nullptr && item.arrow_point_count >= 2 &&
        item.stroke_width > 0.0) {
        QRectF canvasBounds;
        for (const SnowCanvasSceneItem::PenSegmentChunk& chunk : item.penSegmentChunks()) {
            canvasBounds = canvasBounds.isNull() ? chunk.canvasBounds
                                                 : canvasBounds.united(chunk.canvasBounds);
        }
        if (canvasBounds.isEmpty()) {
            return {};
        }
        const QPointF topLeft = canvasToView(projection, canvasBounds.left(), canvasBounds.top());
        const QPointF bottomRight =
            canvasToView(projection, canvasBounds.right(), canvasBounds.bottom());
        QPainterPath boundsPath;
        boundsPath.addRect(QRectF(topLeft, bottomRight).normalized());
        return boundsPath;
    }
    const QPointF center = canvasToView(projection, item.center_x, item.center_y);
    QPainterPath path;
    path.addRect(QRectF(-item.width * projection.cameraZoom / 2.0,
                        -item.height * projection.cameraZoom / 2.0,
                        item.width * projection.cameraZoom, item.height * projection.cameraZoom));
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(item.rotation * kRadiansToDegrees);
    return transform.map(path);
}

snow_canvas_filter_render::Parameters filterParameters(const SceneDisplayInfo& displayInfo,
                                                       const SnowCanvasSceneItem& item,
                                                       qreal devicePixelRatio,
                                                       const QPointF& imageLogicalOrigin) {
    const auto projection = snow_canvas_render_geometry::sceneProjection(displayInfo);
    snow_canvas_filter_render::Parameters parameters;
    parameters.type = item.filter.filter_type;
    parameters.strength = item.filter.strength;
    parameters.logicalBlockSize = item.filter.mosaic_block_size * projection.cameraZoom;
    parameters.logicalSigma = item.filter.blur_sigma * projection.cameraZoom;
    parameters.devicePixelRatio = devicePixelRatio;
    parameters.gridOriginInImage =
        (canvasToView(projection, 0.0, 0.0) - imageLogicalOrigin) * devicePixelRatio;
    return parameters;
}

double filterLogicalSamplingRadius(const SceneDisplayInfo& displayInfo,
                                   const SnowCanvasSceneItem& item) {
    return qMax(0.0, item.filter.sampling_radius) *
           snow_canvas_render_geometry::sceneProjection(displayInfo).cameraZoom;
}

bool effectiveFilterIntersectsViewport(const SceneDisplayInfo& displayInfo,
                                       const SnowCanvasSceneItem& item) {
    const bool validGeometry =
        item.is_free_draw != 0
            ? item.arrow_points != nullptr && item.arrow_point_count >= 2 && item.stroke_width > 0.0
            : item.width > 0.0 && item.height > 0.0;
    if (item.kind != SNOW_SCENE_DISPLAY_ITEM_FILTER || !validGeometry || item.opacity <= 0.0) {
        return false;
    }
    return filterClipPath(displayInfo, item)
        .boundingRect()
        .intersects(QRectF(0.0, 0.0, displayInfo.surface_width, displayInfo.surface_height));
}

struct PhysicalSurfaceGeometry {
    QRect pixelBounds;
    QPointF logicalOrigin;
};

// A cropped filter surface must use one device-pixel-aligned origin for source
// sampling and presentation, otherwise partial and full renders diverge at fractional DPR.
PhysicalSurfaceGeometry physicalSurfaceGeometry(const QRect& logicalBounds,
                                                qreal devicePixelRatio) {
    const int left = static_cast<int>(std::floor(logicalBounds.left() * devicePixelRatio));
    const int top = static_cast<int>(std::floor(logicalBounds.top() * devicePixelRatio));
    const int right = static_cast<int>(std::ceil((logicalBounds.right() + 1) * devicePixelRatio));
    const int bottom = static_cast<int>(std::ceil((logicalBounds.bottom() + 1) * devicePixelRatio));
    return {
        QRect(left, top, right - left, bottom - top),
        QPointF(left / devicePixelRatio, top / devicePixelRatio),
    };
}

QRect physicalRectForLogicalBounds(const QRectF& logicalBounds, const QRect& surfacePixelBounds,
                                   qreal devicePixelRatio) {
    const int left = qMax(0, static_cast<int>(std::floor(logicalBounds.left() * devicePixelRatio)) -
                                 surfacePixelBounds.left());
    const int top = qMax(0, static_cast<int>(std::floor(logicalBounds.top() * devicePixelRatio)) -
                                surfacePixelBounds.top());
    const int right = qMin(surfacePixelBounds.width(),
                           static_cast<int>(std::ceil(logicalBounds.right() * devicePixelRatio)) -
                               surfacePixelBounds.left());
    const int bottom = qMin(surfacePixelBounds.height(),
                            static_cast<int>(std::ceil(logicalBounds.bottom() * devicePixelRatio)) -
                                surfacePixelBounds.top());
    return QRect(QPoint(left, top), QPoint(right - 1, bottom - 1));
}

bool nearlyEqual(double first, double second) {
    const double scale = qMax(1.0, qMax(std::abs(first), std::abs(second)));
    return std::abs(first - second) <= scale * 1e-8;
}

bool isColorEffect(std::uint32_t type) {
    return type == 2 || type == 3;
}

double colorFilterCoverage(const SnowCanvasSceneItem& item) {
    return qBound(0.0, item.opacity, 1.0);
}

struct SparseMaskScan {
    std::vector<snow_canvas_filter_render::MaskSpan> spans;
    std::vector<QRect> occupiedBlocks;
    std::size_t coveredPixels = 0;
};

SparseMaskScan scanSparseMask(const QImage& mask, const QPoint& origin) {
    SparseMaskScan result;
    const auto blockIndex = [](int coordinate) {
        return coordinate >= 0 ? coordinate / 64 : -((-coordinate + 63) / 64);
    };
    const int firstBlockColumn = blockIndex(origin.x());
    const int firstBlockRow = blockIndex(origin.y());
    const int blockColumnCount = blockIndex(origin.x() + mask.width() - 1) - firstBlockColumn + 1;
    const int blockRowCount = blockIndex(origin.y() + mask.height() - 1) - firstBlockRow + 1;
    std::vector<std::uint8_t> occupied(static_cast<std::size_t>(blockColumnCount) * blockRowCount,
                                       0);
    for (int localY = 0; localY < mask.height(); ++localY) {
        const auto* alpha = mask.constScanLine(localY);
        int localX = 0;
        while (localX < mask.width()) {
            while (localX < mask.width() && alpha[localX] == 0) {
                ++localX;
            }
            if (localX == mask.width()) {
                break;
            }
            const int begin = localX;
            while (localX < mask.width() && alpha[localX] != 0) {
                ++localX;
            }
            result.coveredPixels += static_cast<std::size_t>(localX - begin);
            result.spans.push_back(snow_canvas_filter_render::MaskSpan{
                origin.y() + localY,
                origin.x() + begin,
                origin.x() + localX,
            });
            const int y = origin.y() + localY;
            const int row = blockIndex(y) - firstBlockRow;
            const int firstBlock = blockIndex(origin.x() + begin);
            const int lastBlock = blockIndex(origin.x() + localX - 1);
            for (int block = firstBlock; block <= lastBlock; ++block) {
                occupied[static_cast<std::size_t>(row) * blockColumnCount + block -
                         firstBlockColumn] = 1;
            }
        }
    }
    for (int row = 0; row < blockRowCount; ++row) {
        for (int column = 0; column < blockColumnCount; ++column) {
            if (occupied[static_cast<std::size_t>(row) * blockColumnCount + column] != 0) {
                result.occupiedBlocks.emplace_back((firstBlockColumn + column) * 64,
                                                   (firstBlockRow + row) * 64, 64, 64);
            }
        }
    }
    return result;
}

int floorTileCoordinate(int pixel) {
    const int quotient = pixel / snow_canvas_pen_mask::kTileSize;
    const int remainder = pixel % snow_canvas_pen_mask::kTileSize;
    return remainder < 0 ? quotient - 1 : quotient;
}

SparseMaskScan compositePenTilesIntoMask(
    QImage& mask, const QRect& maskPixels, const QRect& surfacePixelBounds,
    const std::vector<std::uint32_t>& indices, const SnowCanvasSceneItem* sceneItems,
    const SceneDisplayInfo& displayInfo, qreal devicePixelRatio,
    snow_canvas_pen_mask::PenMaskAtlas& atlas,
    const snow_canvas_filter_render::ExecutionOptions& execution, bool collectMetadata) {
    SparseMaskScan scan;
    const QRect globalMask = maskPixels.translated(surfacePixelBounds.topLeft());
    const int firstTileX = floorTileCoordinate(globalMask.left());
    const int lastTileX = floorTileCoordinate(globalMask.right());
    const int firstTileY = floorTileCoordinate(globalMask.top());
    const int lastTileY = floorTileCoordinate(globalMask.bottom());
    std::array<std::uint8_t, snow_canvas_pen_mask::kTileSize * snow_canvas_pen_mask::kTileSize>
        groupAlpha{};
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            groupAlpha.fill(0);
            bool occupied = false;
            for (std::uint32_t index : indices) {
                const SnowCanvasSceneItem& item = sceneItems[index];
                if (item.is_free_draw == 0) {
                    continue;
                }
                const std::shared_ptr<const snow_canvas_pen_mask::Tile> tile =
                    atlas.tile(item, tileX, tileY, displayInfo, devicePixelRatio, execution);
                if (!tile || !tile->occupied) {
                    continue;
                }
                occupied = true;
                for (const snow_canvas_pen_mask::RowSpan& span : tile->spans) {
                    const auto* source = tile->alpha.constScanLine(span.y);
                    auto* destination = groupAlpha.data() + static_cast<std::size_t>(span.y) *
                                                                snow_canvas_pen_mask::kTileSize;
                    for (int x = span.beginX; x < span.endX; ++x) {
                        const int alpha = source[x];
                        destination[x] = static_cast<std::uint8_t>(
                            alpha + (destination[x] * (255 - alpha) + 127) / 255);
                    }
                }
            }
            if (!occupied) {
                continue;
            }
            const QRect tileGlobal(
                tileX * snow_canvas_pen_mask::kTileSize, tileY * snow_canvas_pen_mask::kTileSize,
                snow_canvas_pen_mask::kTileSize, snow_canvas_pen_mask::kTileSize);
            const QRect overlap = tileGlobal.intersected(globalMask);
            if (overlap.isEmpty()) {
                continue;
            }
            bool tileHasCoverage = false;
            for (int globalY = overlap.top(); globalY <= overlap.bottom(); ++globalY) {
                const int tileLocalY = globalY - tileGlobal.top();
                const int maskLocalY = globalY - globalMask.top();
                auto* destination = mask.scanLine(maskLocalY);
                const auto* source = groupAlpha.data() + static_cast<std::size_t>(tileLocalY) *
                                                             snow_canvas_pen_mask::kTileSize;
                int globalX = overlap.left();
                while (globalX <= overlap.right()) {
                    const int tileLocalX = globalX - tileGlobal.left();
                    const int alpha = source[tileLocalX];
                    if (alpha == 0) {
                        ++globalX;
                        continue;
                    }
                    const int beginGlobalX = globalX;
                    while (globalX <= overlap.right()) {
                        const int sourceX = globalX - tileGlobal.left();
                        const int destinationX = globalX - globalMask.left();
                        const int sourceAlpha = source[sourceX];
                        if (sourceAlpha == 0) {
                            break;
                        }
                        destination[destinationX] = static_cast<std::uint8_t>(
                            sourceAlpha +
                            (destination[destinationX] * (255 - sourceAlpha) + 127) / 255);
                        ++globalX;
                    }
                    tileHasCoverage = true;
                    if (collectMetadata) {
                        scan.spans.push_back(snow_canvas_filter_render::MaskSpan{
                            maskPixels.top() + maskLocalY,
                            maskPixels.left() + beginGlobalX - globalMask.left(),
                            maskPixels.left() + globalX - globalMask.left(),
                        });
                        scan.coveredPixels += static_cast<std::size_t>(globalX - beginGlobalX);
                    }
                }
            }
            if (collectMetadata && tileHasCoverage) {
                scan.occupiedBlocks.push_back(overlap.translated(-surfacePixelBounds.topLeft()));
            }
        }
    }
    return scan;
}

bool applyPenTilesDirect(const QImage& source, QImage& destination, const QRect& maskPixels,
                         const QRect& surfacePixelBounds, const std::vector<std::uint32_t>& indices,
                         const SnowCanvasSceneItem* sceneItems, const SceneDisplayInfo& displayInfo,
                         qreal devicePixelRatio, snow_canvas_pen_mask::PenMaskAtlas& atlas,
                         const snow_canvas_filter_render::Parameters& parameters,
                         snow_canvas_filter_render::RenderWorkspace& workspace,
                         const snow_canvas_filter_render::ExecutionOptions& execution,
                         std::size_t* coveredPixels, std::size_t* boundingPixels) {
    const QRect globalMask = maskPixels.translated(surfacePixelBounds.topLeft());
    const int firstTileX = floorTileCoordinate(globalMask.left());
    const int lastTileX = floorTileCoordinate(globalMask.right());
    const int firstTileY = floorTileCoordinate(globalMask.top());
    const int lastTileY = floorTileCoordinate(globalMask.bottom());
    std::array<std::uint8_t, snow_canvas_pen_mask::kTileSize * snow_canvas_pen_mask::kTileSize>
        groupAlpha{};
    bool appliedAny = false;
    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            groupAlpha.fill(0);
            bool occupied = false;
            for (std::uint32_t index : indices) {
                const std::shared_ptr<const snow_canvas_pen_mask::Tile> tile = atlas.tile(
                    sceneItems[index], tileX, tileY, displayInfo, devicePixelRatio, execution);
                if (!tile || !tile->occupied) {
                    continue;
                }
                occupied = true;
                for (const snow_canvas_pen_mask::RowSpan& span : tile->spans) {
                    const auto* sourceAlpha = tile->alpha.constScanLine(span.y);
                    auto* destinationAlpha =
                        groupAlpha.data() +
                        static_cast<std::size_t>(span.y) * snow_canvas_pen_mask::kTileSize;
                    for (int x = span.beginX; x < span.endX; ++x) {
                        const int alpha = sourceAlpha[x];
                        destinationAlpha[x] = static_cast<std::uint8_t>(
                            alpha + (destinationAlpha[x] * (255 - alpha) + 127) / 255);
                    }
                }
            }
            if (!occupied) {
                continue;
            }
            const QRect tileGlobal(
                tileX * snow_canvas_pen_mask::kTileSize, tileY * snow_canvas_pen_mask::kTileSize,
                snow_canvas_pen_mask::kTileSize, snow_canvas_pen_mask::kTileSize);
            const QRect overlap = tileGlobal.intersected(globalMask);
            if (overlap.isEmpty()) {
                continue;
            }
            const QPoint maskOrigin = tileGlobal.topLeft() - surfacePixelBounds.topLeft();
            const QRect destinationPixels = overlap.translated(-surfacePixelBounds.topLeft());
            std::vector<snow_canvas_filter_render::MaskSpan> spans;
            std::size_t tileCoveredPixels = 0;
            for (int globalY = overlap.top(); globalY <= overlap.bottom(); ++globalY) {
                const int localY = globalY - tileGlobal.top();
                const auto* alpha = groupAlpha.data() + static_cast<std::size_t>(localY) *
                                                            snow_canvas_pen_mask::kTileSize;
                int globalX = overlap.left();
                while (globalX <= overlap.right()) {
                    while (globalX <= overlap.right() && alpha[globalX - tileGlobal.left()] == 0) {
                        ++globalX;
                    }
                    if (globalX > overlap.right()) {
                        break;
                    }
                    const int begin = globalX;
                    while (globalX <= overlap.right() && alpha[globalX - tileGlobal.left()] != 0) {
                        ++globalX;
                    }
                    spans.push_back(snow_canvas_filter_render::MaskSpan{
                        globalY - surfacePixelBounds.top(),
                        begin - surfacePixelBounds.left(),
                        globalX - surfacePixelBounds.left(),
                    });
                    tileCoveredPixels += static_cast<std::size_t>(globalX - begin);
                }
            }
            if (spans.empty()) {
                continue;
            }
            const QImage tileMask(groupAlpha.data(), snow_canvas_pen_mask::kTileSize,
                                  snow_canvas_pen_mask::kTileSize, snow_canvas_pen_mask::kTileSize,
                                  QImage::Format_Alpha8);
            const std::vector<QRect> occupiedBlocks{destinationPixels};
            bool tileApplied = false;
            if (!execution.forceDenseMask) {
                tileApplied = snow_canvas_filter_render::applyMaskedSparse(
                    source, destination, tileMask, maskOrigin, destinationPixels, spans,
                    occupiedBlocks, parameters, &workspace, execution);
            }
            if (!tileApplied) {
                tileApplied = snow_canvas_filter_render::applyMasked(
                    source, destination, tileMask, maskOrigin, destinationPixels, parameters,
                    &workspace, execution);
            }
            if (!tileApplied) {
                return false;
            }
            appliedAny = true;
            if (coveredPixels != nullptr) {
                *coveredPixels += tileCoveredPixels;
            }
            if (boundingPixels != nullptr) {
                *boundingPixels += static_cast<std::size_t>(overlap.width()) * overlap.height();
            }
        }
    }
    return appliedAny;
}

const SnowCanvasSceneItem* textItemForHoverOverlay(const SnowCanvasOverlayItem& overlay,
                                                   const SnowCanvasSceneItem* sceneItems,
                                                   std::uint32_t sceneItemCount) {
    if (overlay.rect_kind != SNOW_OVERLAY_RECT_TEXT_HOVER_UNDERLINE || sceneItems == nullptr) {
        return nullptr;
    }
    // Hit testing resolves the topmost item, which is the last matching scene
    // item when otherwise-identical text elements overlap.
    for (std::uint32_t index = sceneItemCount; index > 0; --index) {
        const SnowCanvasSceneItem& item = sceneItems[index - 1];
        if (item.kind == SNOW_SCENE_DISPLAY_ITEM_TEXT &&
            nearlyEqual(item.center_x, overlay.center_x) &&
            nearlyEqual(item.center_y, overlay.center_y) &&
            nearlyEqual(item.width, overlay.width) && nearlyEqual(item.height, overlay.height) &&
            nearlyEqual(item.rotation, overlay.rotation)) {
            return &item;
        }
    }
    return nullptr;
}

bool drawTextHoverOverlay(QPainter& painter, const SceneDisplayInfo* sceneDisplayInfo,
                          const SnowCanvasSceneItem* sceneItems, std::uint32_t sceneItemCount,
                          const SnowCanvasOverlayItem& overlay) {
    if (sceneDisplayInfo == nullptr) {
        return false;
    }
    const SnowCanvasSceneItem* item = textItemForHoverOverlay(overlay, sceneItems, sceneItemCount);
    if (item == nullptr) {
        return false;
    }

    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::sceneProjection(*sceneDisplayInfo);
    const double zoom = projection.cameraZoom;
    if (!(zoom > 0.0)) {
        return false;
    }
    const QPointF center = canvasToView(projection, item->center_x, item->center_y);
    const QRectF localRect(-item->width * zoom / 2.0, -item->height * zoom / 2.0,
                           item->width * zoom, item->height * zoom);

    painter.save();
    painter.translate(center);
    painter.rotate(item->rotation * kRadiansToDegrees);
    snow_canvas_text_render::drawHoverUnderlines(painter, *item, painter.font(), localRect, zoom,
                                                 toQColor(overlay.stroke),
                                                 overlay.stroke_width * zoom);
    painter.restore();
    return true;
}

} // namespace

namespace {
void renderSceneItemsImpl(const SceneRenderRequest& request);
}

QColor toQColor(const SnowColorRgba8& color) {
    return QColor(color.r, color.g, color.b, color.a);
}

void renderSceneItems(const SceneRenderRequest& request) {
    if (request.painter == nullptr || request.displayInfo == nullptr) {
        return;
    }
    renderSceneItemsImpl(request);
    if (request.diagnostics != nullptr) {
        *request.diagnostics = g_filterDiagnostics;
    }
}

void renderSceneItemsTiled(const SceneRenderRequest& request) {
    if (request.painter == nullptr || request.displayInfo == nullptr ||
        request.sceneItems == nullptr || request.sceneItemCount == 0 ||
        request.exposedRegion.isEmpty() || request.cacheNamespace == nullptr) {
        renderSceneItems(request);
        return;
    }

    const qreal dpr = request.painter->device() != nullptr
                          ? qMax<qreal>(1.0, request.painter->device()->devicePixelRatioF())
                          : 1.0;
    const QSize logicalSize(qMax(1, qRound(request.displayInfo->surface_width)),
                            qMax(1, qRound(request.displayInfo->surface_height)));
    const QSize physicalSize(qCeil(logicalSize.width() * dpr), qCeil(logicalSize.height() * dpr));
    if (physicalSize.isEmpty()) {
        return;
    }

    std::vector<std::uint32_t> filters;
    for (std::uint32_t index = 0; index < request.sceneItemCount; ++index) {
        if (request.sceneItems[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER &&
            request.sceneItems[index].opacity > 0.0) {
            filters.push_back(index);
        }
    }
    if (filters.empty()) {
        renderSceneItems(request);
        return;
    }

    FilterRenderDiagnostics aggregateDiagnostics;
    const QRect exposedBounds = request.exposedRegion.boundingRect();
    const int firstTileX = std::max(0, qFloor(exposedBounds.left() * dpr) /
                                         snow_canvas_filter_tile_cache::kTilePhysicalSize);
    const int firstTileY = std::max(0, qFloor(exposedBounds.top() * dpr) /
                                         snow_canvas_filter_tile_cache::kTilePhysicalSize);
    const int lastTileX = std::max(firstTileX, qFloor(exposedBounds.right() * dpr) /
                                                     snow_canvas_filter_tile_cache::kTilePhysicalSize);
    const int lastTileY = std::max(firstTileY, qFloor(exposedBounds.bottom() * dpr) /
                                                     snow_canvas_filter_tile_cache::kTilePhysicalSize);
    const std::uint64_t fullFingerprint =
        filterDependencyFingerprint(request.sceneItems, request.sceneItemCount);

    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const QRect physicalRect(
                tileX * snow_canvas_filter_tile_cache::kTilePhysicalSize,
                tileY * snow_canvas_filter_tile_cache::kTilePhysicalSize,
                std::min(snow_canvas_filter_tile_cache::kTilePhysicalSize,
                         physicalSize.width() - tileX * snow_canvas_filter_tile_cache::kTilePhysicalSize),
                std::min(snow_canvas_filter_tile_cache::kTilePhysicalSize,
                         physicalSize.height() - tileY * snow_canvas_filter_tile_cache::kTilePhysicalSize));
            if (physicalRect.isEmpty()) {
                continue;
            }
            const QRect logicalRect(
                qFloor(physicalRect.left() / dpr), qFloor(physicalRect.top() / dpr),
                qCeil(physicalRect.width() / dpr), qCeil(physicalRect.height() / dpr));
            QImage tileImage;
            try {
                tileImage = QImage(physicalRect.size(), QImage::Format_ARGB32_Premultiplied);
            } catch (const std::bad_alloc&) {
                continue;
            }
            if (tileImage.isNull()) {
                continue;
            }
            tileImage.setDevicePixelRatio(dpr);
            tileImage.fill(request.clearBackgroundEnabled
                               ? toQColor(request.displayInfo->clear_color)
                               : Qt::transparent);
            QPainter tilePainter(&tileImage);
            tilePainter.setRenderHints(request.painter->renderHints());
            tilePainter.translate(-physicalRect.left() / dpr, -physicalRect.top() / dpr);
            tilePainter.setClipRegion(QRegion(logicalRect));
            SceneRenderRequest tiled = request;
            tiled.painter = &tilePainter;
            tiled.exposedRegion = QRegion(logicalRect);
            SnowCanvasRenderContext tileContext;
            if (request.backgroundContext != nullptr) {
                tileContext = *request.backgroundContext;
                tileContext.exposedRegion = QRegion(logicalRect);
                tileContext.devicePixelRatio = dpr;
                tiled.backgroundContext = &tileContext;
            }
            tiled.enableFilterTileCache = true;
            tiled.filterTileCoordinate = QPoint(tileX, tileY);
            tiled.filterTileContentKey = request.filterTileContentKey != 0
                                             ? request.filterTileContentKey
                                             : fullFingerprint;
            renderSceneItemsImpl(tiled);
            accumulateFilterRenderDiagnostics(aggregateDiagnostics,
                                              filterRenderDiagnosticsForCurrentThread());
            tilePainter.end();
            request.painter->save();
            request.painter->setClipRegion(request.exposedRegion, Qt::IntersectClip);
            request.painter->drawImage(QPointF(physicalRect.left() / dpr,
                                               physicalRect.top() / dpr),
                                       tileImage);
            request.painter->restore();
        }
    }

    const auto retainedDiagnostics = snow_canvas_filter_tile_cache::takeDiagnostics();
    g_filterDiagnostics = aggregateDiagnostics;
    g_filterDiagnostics.sourceTileHits += retainedDiagnostics.hits;
    g_filterDiagnostics.sourceTileMisses += retainedDiagnostics.misses;
    g_filterDiagnostics.sourceTileEvictions += retainedDiagnostics.evictions;
    g_filterDiagnostics.sourceTileCandidates += retainedDiagnostics.hits + retainedDiagnostics.misses;
    g_filterDiagnostics.sourceTileVisits += retainedDiagnostics.hits + retainedDiagnostics.misses;
    g_filterDiagnostics.sourceDependencyInvalidations += retainedDiagnostics.dependencyInvalidations;
    g_filterDiagnostics.retainedSourceBytes = retainedDiagnostics.retainedBytes;
    if (request.diagnostics != nullptr) {
        *request.diagnostics = g_filterDiagnostics;
    }
}

namespace {
void renderSceneItemsImpl(const SceneRenderRequest& request) {
    QPainter& painter = *request.painter;
    const SceneDisplayInfo& displayInfo = *request.displayInfo;
    const SnowCanvasSceneItem* sceneItems = request.sceneItems;
    const std::uint32_t sceneItemCount = request.sceneItemCount;
    const QRegion& exposedRegion = request.exposedRegion;
    const std::uint32_t* candidateIndices = request.candidateIndices;
    const std::uint32_t candidateCount = request.candidateCount;
    const QImage* backgroundImage = request.backgroundImage;
    SnowCanvasCustomRenderer* backgroundRenderer = request.backgroundRenderer;
    const SnowCanvasRenderContext* backgroundContext = request.backgroundContext;
    const SnowCanvasDisplayCache* displayCache = request.displayCache;
    const void* penMaskNamespace = request.cacheNamespace != nullptr
                                       ? request.cacheNamespace
                                       : static_cast<const void*>(displayCache);
    snow_canvas_filter_render::RenderWorkspace* renderWorkspace = request.workspace;
    const snow_canvas_filter_render::ExecutionOptions& execution = request.execution;
    g_filterDiagnostics = {};
    if (sceneItems == nullptr) {
        if (backgroundImage != nullptr && !backgroundImage->isNull()) {
            painter.drawImage(
                QRectF(0.0, 0.0, displayInfo.surface_width, displayInfo.surface_height),
                *backgroundImage);
        }
        if (backgroundRenderer != nullptr && backgroundContext != nullptr) {
            painter.save();
            backgroundRenderer->renderBeforeCanvas(painter, *backgroundContext);
            painter.restore();
        }
        return;
    }
    for (const QRect& rect : exposedRegion) {
        g_filterDiagnostics.exposedPixelCount +=
            static_cast<std::size_t>(rect.width()) * static_cast<std::size_t>(rect.height());
    }
    const bool hasBackgroundContent =
        (backgroundImage != nullptr && !backgroundImage->isNull()) ||
        (backgroundRenderer != nullptr && backgroundContext != nullptr);
    const qreal devicePixelRatio =
        painter.device() != nullptr ? qMax<qreal>(1.0, painter.device()->devicePixelRatioF()) : 1.0;
    thread_local snow_canvas_pen_mask::PenMaskAtlas fallbackPenMaskAtlas;
    snow_canvas_pen_mask::PenMaskAtlas& penMaskAtlas =
        request.penMaskAtlas != nullptr ? *request.penMaskAtlas : fallbackPenMaskAtlas;
    std::vector<std::uint32_t> filterIndices;
    if (displayCache != nullptr) {
        filterIndices = displayCache->filterIndices();
    } else {
        for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
            if (sceneItems[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                filterIndices.push_back(index);
            }
        }
    }
    const bool hasPenFilterItems =
        std::any_of(filterIndices.begin(), filterIndices.end(),
                    [sceneItems, sceneItemCount](std::uint32_t index) {
                        return index < sceneItemCount && sceneItems[index].is_free_draw != 0;
                    });
    if (hasPenFilterItems) {
        penMaskAtlas.beginFrame(penMaskNamespace, displayInfo, devicePixelRatio, displayCache);
    }
    struct CachedFilterFrameInfo {
        bool effective = false;
        bool axisAlignedRect = false;
        bool devicePixelAlignedRect = false;
        bool pathReady = false;
        QPainterPath clipPath;
        QRectF logicalBounds;
    };
    std::vector<int> cachedFilterSlots(sceneItemCount, -1);
    std::vector<CachedFilterFrameInfo> cachedFilters;
    cachedFilters.reserve(filterIndices.size());
    const QRectF viewportBounds(0.0, 0.0, displayInfo.surface_width, displayInfo.surface_height);
    for (std::uint32_t index : filterIndices) {
        if (index >= sceneItemCount) {
            continue;
        }
        const SnowCanvasSceneItem& item = sceneItems[index];
        cachedFilterSlots[index] = static_cast<int>(cachedFilters.size());
        cachedFilters.emplace_back();
        CachedFilterFrameInfo& cached = cachedFilters.back();
        item.takePenFilterGeometryDiagnostics(&g_filterDiagnostics.penGeometryChunkBuildCount,
                                              &g_filterDiagnostics.penGeometryChunkReuseCount);
        const bool validGeometry = item.is_free_draw != 0
                                       ? item.arrow_points != nullptr &&
                                             item.arrow_point_count >= 2 && item.stroke_width > 0.0
                                       : item.width > 0.0 && item.height > 0.0;
        cached.effective =
            item.kind == SNOW_SCENE_DISPLAY_ITEM_FILTER && validGeometry && item.opacity > 0.0;
        if (!cached.effective) {
            continue;
        }
        if (displayCache != nullptr) {
            cached.logicalBounds = item.viewBounds;
        } else {
            const StageTimer pathTimer{g_filterDiagnostics.pathConstructionNanoseconds};
            cached.clipPath = filterClipPath(displayInfo, item);
            cached.pathReady = true;
            cached.logicalBounds = cached.clipPath.boundingRect();
        }
        cached.axisAlignedRect = item.is_free_draw == 0 && std::abs(item.rotation) <= 1e-12;
        const double physicalLeft = cached.logicalBounds.left() * devicePixelRatio;
        const double physicalTop = cached.logicalBounds.top() * devicePixelRatio;
        const double physicalRight =
            (cached.logicalBounds.left() + cached.logicalBounds.width()) * devicePixelRatio;
        const double physicalBottom =
            (cached.logicalBounds.top() + cached.logicalBounds.height()) * devicePixelRatio;
        cached.devicePixelAlignedRect = cached.axisAlignedRect &&
                                        nearlyEqual(physicalLeft, std::round(physicalLeft)) &&
                                        nearlyEqual(physicalTop, std::round(physicalTop)) &&
                                        nearlyEqual(physicalRight, std::round(physicalRight)) &&
                                        nearlyEqual(physicalBottom, std::round(physicalBottom));
        cached.effective = cached.logicalBounds.intersects(viewportBounds);
    }
    const auto cachedFilter = [&](std::uint32_t index) -> const CachedFilterFrameInfo& {
        return cachedFilters[static_cast<std::size_t>(cachedFilterSlots[index])];
    };
    const auto filterPath = [&](std::uint32_t index) -> const QPainterPath& {
        CachedFilterFrameInfo& cached =
            cachedFilters[static_cast<std::size_t>(cachedFilterSlots[index])];
        if (!cached.pathReady) {
            const StageTimer pathTimer{g_filterDiagnostics.pathConstructionNanoseconds};
            cached.clipPath = filterClipPath(displayInfo, sceneItems[index]);
            cached.pathReady = true;
        }
        return cached.clipPath;
    };
    bool hasFilter = false;
    for (std::uint32_t index : filterIndices) {
        if (index < sceneItemCount && cachedFilter(index).effective &&
            exposedRegion.intersects(cachedFilter(index).logicalBounds.toAlignedRect())) {
            hasFilter = true;
            break;
        }
    }
    if (hasFilter && (hasBackgroundContent || sceneItemCount > filterIndices.size())) {
        g_filterDiagnostics.usedFilterPath = true;
        g_filterDiagnostics.recorderCount = 1;
        const QRect viewportRect(0, 0, qMax(1, qRound(displayInfo.surface_width)),
                                 qMax(1, qRound(displayInfo.surface_height)));
        struct PlannedComponent {
            QRect bounds;
            std::vector<std::pair<std::uint32_t, QRect>> layerDestinations;
        };
        std::vector<PlannedComponent> components;
        for (const QRect& rect : exposedRegion) {
            const QRect clipped = rect.intersected(viewportRect);
            if (!clipped.isEmpty()) {
                components.push_back(PlannedComponent{clipped, {}});
            }
        }
        const auto mergeComponents = [&components] {
            bool merged = false;
            do {
                merged = false;
                for (std::size_t left = 0; left < components.size() && !merged; ++left) {
                    for (std::size_t right = left + 1; right < components.size(); ++right) {
                        if (components[left].bounds.intersects(components[right].bounds)) {
                            components[left].bounds =
                                components[left].bounds.united(components[right].bounds);
                            for (const auto& [layer, destination] :
                                 components[right].layerDestinations) {
                                auto found = std::find_if(
                                    components[left].layerDestinations.begin(),
                                    components[left].layerDestinations.end(),
                                    [layer](const auto& entry) { return entry.first == layer; });
                                if (found == components[left].layerDestinations.end()) {
                                    components[left].layerDestinations.emplace_back(layer,
                                                                                    destination);
                                } else {
                                    found->second = found->second.united(destination);
                                }
                            }
                            components.erase(components.begin() +
                                             static_cast<std::ptrdiff_t>(right));
                            merged = true;
                            break;
                        }
                    }
                }
            } while (merged);
        };
        mergeComponents();

        // Filters in one adjacent run share a pre-layer source, so their halos
        // expand the entering component independently rather than recursively.
        for (std::size_t reverse = filterIndices.size(); reverse > 0;) {
            const std::size_t layerEnd = reverse;
            std::size_t layerStart = reverse - 1;
            while (layerStart > 0 &&
                   filterIndices[layerStart - 1] + 1 == filterIndices[layerStart]) {
                --layerStart;
            }
            for (std::size_t componentIndex = 0; componentIndex < components.size();
                 ++componentIndex) {
                PlannedComponent& component = components[componentIndex];
                const QRect entering = component.bounds;
                component.layerDestinations.emplace_back(filterIndices[layerStart], entering);
                QRect expanded = entering;
                for (std::size_t filterPosition = layerStart; filterPosition < layerEnd;
                     ++filterPosition) {
                    const std::uint32_t index = filterIndices[filterPosition];
                    if (index >= sceneItemCount) {
                        continue;
                    }
                    const SnowCanvasSceneItem& filter = sceneItems[index];
                    if (!cachedFilter(index).effective) {
                        continue;
                    }
                    const QRect filterBounds = cachedFilter(index).logicalBounds.toAlignedRect();
                    const QRect affected = entering.intersected(filterBounds);
                    if (affected.isEmpty()) {
                        continue;
                    }
                    const snow_canvas_filter_render::Parameters plannedParameters =
                        filterParameters(displayInfo, filter, devicePixelRatio, {});
                    const int radius =
                        filter.filter.filter_type == 1
                            ? qCeil(snow_canvas_filter_render::samplingRadiusPixels(
                                        plannedParameters) /
                                    devicePixelRatio)
                            : qCeil(filterLogicalSamplingRadius(displayInfo, filter));
                    expanded = expanded.united(affected.adjusted(-radius, -radius, radius, radius)
                                                   .intersected(viewportRect));
                }
                component.bounds = expanded;
            }
            mergeComponents();
            reverse = layerStart;
        }

        thread_local snow_canvas_filter_render::RenderWorkspace fallbackWorkspace;
        snow_canvas_filter_render::RenderWorkspace& workspace =
            renderWorkspace != nullptr ? *renderWorkspace : fallbackWorkspace;
        workspace.resetDiagnostics();
        g_filterDiagnostics.surfaceComponentCount = components.size();

        struct EffectGroup {
            std::uint32_t type = 0;
            int blockPixels = 0;
            snow_canvas_filter_render::GaussianBlurPlan gaussianPlan;
            std::vector<std::uint32_t> indices;
        };

        for (const PlannedComponent& component : components) {
            const QRect surfaceBounds = component.bounds;
            if (surfaceBounds.isEmpty()) {
                continue;
            }
            const PhysicalSurfaceGeometry surfaceGeometry =
                physicalSurfaceGeometry(surfaceBounds, devicePixelRatio);
            QImage& scene =
                workspace.sceneScratch(surfaceGeometry.pixelBounds.size(), devicePixelRatio);
            if (scene.isNull()) {
                continue;
            }
            bool copiedBackgroundRows = false;
            if (backgroundImage != nullptr && backgroundRenderer == nullptr &&
                backgroundImage->format() == QImage::Format_ARGB32_Premultiplied &&
                qFuzzyCompare(backgroundImage->devicePixelRatio(), devicePixelRatio) &&
                backgroundImage->width() == qCeil(viewportRect.width() * devicePixelRatio) &&
                backgroundImage->height() == qCeil(viewportRect.height() * devicePixelRatio)) {
                const int sourceLeft = surfaceGeometry.pixelBounds.left();
                const int sourceTop = surfaceGeometry.pixelBounds.top();
                if (sourceLeft >= 0 && sourceTop >= 0 &&
                    sourceLeft + scene.width() <= backgroundImage->width() &&
                    sourceTop + scene.height() <= backgroundImage->height()) {
                    const std::size_t rowBytes =
                        static_cast<std::size_t>(scene.width()) * sizeof(QRgb);
                    for (int y = 0; y < scene.height(); ++y) {
                        std::memcpy(scene.scanLine(y),
                                    backgroundImage->constScanLine(sourceTop + y) +
                                        static_cast<qsizetype>(sourceLeft) * sizeof(QRgb),
                                    rowBytes);
                    }
                    g_filterDiagnostics.copiedBytes += rowBytes * scene.height();
                    copiedBackgroundRows = true;
                }
            }
            if (!copiedBackgroundRows) {
                scene.fill(request.enableFilterTileCache ? toQColor(displayInfo.clear_color)
                                                          : Qt::transparent);
            }
            const std::size_t workingPixels =
                static_cast<std::size_t>(scene.width()) * static_cast<std::size_t>(scene.height());
            g_filterDiagnostics.totalWorkingPixelCount += workingPixels;
            g_filterDiagnostics.peakWorkingPixelCount =
                std::max(g_filterDiagnostics.peakWorkingPixelCount, workingPixels);

            std::vector<std::uint32_t> stream;
            if (displayCache != nullptr) {
                displayCache->sceneCandidateIndices(QRegion(surfaceBounds), &stream);
                g_filterDiagnostics.spatialCandidateCount += stream.size();
            } else if (candidateIndices != nullptr) {
                for (std::uint32_t candidate = 0; candidate < candidateCount; ++candidate) {
                    const std::uint32_t index = candidateIndices[candidate];
                    if (index < sceneItemCount &&
                        sceneItemBounds(displayInfo, sceneItems[index]).intersects(surfaceBounds)) {
                        stream.push_back(index);
                    }
                }
                g_filterDiagnostics.spatialCandidateCount += stream.size();
            } else {
                for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
                    if (sceneItemBounds(displayInfo, sceneItems[index]).intersects(surfaceBounds)) {
                        stream.push_back(index);
                    }
                }
            }
            std::vector<std::uint32_t> expandedStream = stream;
            for (std::uint32_t index : stream) {
                if (index >= sceneItemCount ||
                    sceneItems[index].kind != SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                    continue;
                }
                std::uint32_t begin = index;
                std::uint32_t end = index + 1;
                while (begin > 0 && sceneItems[begin - 1].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                    --begin;
                }
                while (end < sceneItemCount &&
                       sceneItems[end].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                    ++end;
                }
                for (std::uint32_t adjacent = begin; adjacent < end; ++adjacent) {
                    expandedStream.push_back(adjacent);
                }
            }
            std::sort(expandedStream.begin(), expandedStream.end());
            expandedStream.erase(std::unique(expandedStream.begin(), expandedStream.end()),
                                 expandedStream.end());

            QPainter scenePainter(&scene);
            scenePainter.setFont(painter.font());
            scenePainter.setRenderHints(painter.renderHints());
            scenePainter.translate(-surfaceGeometry.logicalOrigin);
            std::size_t replayStartPosition = 0;
            bool reusedPreLayer = false;
            bool preloadedLookupAttempted = false;
            std::uint32_t preloadedLayerStart = std::numeric_limits<std::uint32_t>::max();
            std::shared_ptr<const snow_canvas_filter_tile_cache::Entry> preloadedSource;
            if (request.enableFilterTileCache && request.cacheNamespace != nullptr) {
                for (std::size_t candidatePosition = 0; candidatePosition < expandedStream.size();
                     ++candidatePosition) {
                    const std::uint32_t candidateIndex = expandedStream[candidatePosition];
                    if (candidateIndex >= sceneItemCount ||
                        sceneItems[candidateIndex].kind != SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                        continue;
                    }
                    std::uint32_t candidateLayerStart = candidateIndex;
                    while (candidateLayerStart > 0 &&
                           sceneItems[candidateLayerStart - 1].kind ==
                               SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                        --candidateLayerStart;
                    }
                    const QRect sourcePhysicalBounds = surfaceGeometry.pixelBounds;
                    const snow_canvas_filter_tile_cache::Key sourceKey{
                        request.cacheNamespace,
                        request.filterTileCoordinate,
                        sourcePhysicalBounds,
                        QSize(qMax(1, qRound(displayInfo.surface_width)),
                             qMax(1, qRound(displayInfo.surface_height))),
                        filterTileDprBits(devicePixelRatio),
                        request.filterTileContentKey,
                        filterDependencyFingerprintForRegion(sceneItems, candidateLayerStart,
                                                             displayInfo, surfaceBounds),
                        0,
                    };
                    preloadedLayerStart = candidateLayerStart;
                    preloadedLookupAttempted = true;
                    const auto retainedSource = snow_canvas_filter_tile_cache::find(sourceKey,
                                                                                     nullptr);
                    preloadedSource = retainedSource;
                    if (retainedSource && retainedSource->image.size() == scene.size() &&
                        retainedSource->image.format() == scene.format()) {
                        for (int row = 0; row < scene.height(); ++row) {
                            std::memcpy(scene.scanLine(row), retainedSource->image.constScanLine(row),
                                        static_cast<std::size_t>(scene.bytesPerLine()));
                        }
                        scenePainter.begin(&scene);
                        scenePainter.setFont(painter.font());
                        scenePainter.setRenderHints(painter.renderHints());
                        scenePainter.translate(-surfaceGeometry.logicalOrigin);
                        replayStartPosition = candidatePosition;
                        reusedPreLayer = true;
                    }
                    break;
                }
            }
            const StageTimer backgroundReplayTimer{g_filterDiagnostics.sceneReplayNanoseconds};
            if (!reusedPreLayer && !copiedBackgroundRows && backgroundImage != nullptr &&
                !backgroundImage->isNull()) {
                scenePainter.drawImage(viewportRect, *backgroundImage);
            }
            if (!reusedPreLayer && backgroundRenderer != nullptr && backgroundContext != nullptr) {
                scenePainter.save();
                SnowCanvasRenderContext samplingContext = *backgroundContext;
                // Spatial filters need the backdrop outside the output tile. The planner has
                // already bounded that sampling halo in surfaceBounds, so expose the same area
                // to custom renderers that honor partial-paint requests.
                samplingContext.exposedRegion = QRegion(surfaceBounds);
                backgroundRenderer->renderBeforeCanvas(scenePainter, samplingContext);
                scenePainter.restore();
            }
            bool renderedContent = reusedPreLayer || hasBackgroundContent;
            for (std::size_t position = replayStartPosition; position < expandedStream.size();) {
                const std::uint32_t index = expandedStream[position];
                if (index >= sceneItemCount) {
                    ++position;
                    continue;
                }
                const SnowCanvasSceneItem& item = sceneItems[index];
                if (item.kind != SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                    const StageTimer replayTimer{g_filterDiagnostics.sceneReplayNanoseconds};
                    drawSceneItem(scenePainter, displayInfo, item);
                    ++g_filterDiagnostics.replayedItemCount;
                    renderedContent = true;
                    ++position;
                    continue;
                }

                std::uint32_t layerStart = index;
                std::uint32_t layerEnd = index + 1;
                while (layerStart > 0 &&
                       sceneItems[layerStart - 1].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                    --layerStart;
                }
                while (layerEnd < sceneItemCount &&
                       sceneItems[layerEnd].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
                    ++layerEnd;
                }
                while (position < expandedStream.size() && expandedStream[position] < layerEnd) {
                    ++position;
                }
                if (!renderedContent) {
                    continue;
                }

                std::vector<EffectGroup> groups;
                for (std::uint32_t filterIndex = layerStart; filterIndex < layerEnd;
                     ++filterIndex) {
                    const SnowCanvasSceneItem& filter = sceneItems[filterIndex];
                    if (!cachedFilter(filterIndex).effective ||
                        !cachedFilter(filterIndex).logicalBounds.intersects(surfaceBounds)) {
                        continue;
                    }
                    const auto projection =
                        snow_canvas_render_geometry::sceneProjection(displayInfo);
                    EffectGroup key;
                    key.type = filter.filter.filter_type;
                    key.blockPixels = qMax(1, qRound(filter.filter.mosaic_block_size *
                                                     projection.cameraZoom * devicePixelRatio));
                    if (key.type == 1) {
                        key.gaussianPlan = snow_canvas_filter_render::gaussianBlurPlan(
                            filterParameters(displayInfo, filter, devicePixelRatio, {}));
                    }
                    auto found =
                        std::find_if(groups.begin(), groups.end(), [&](const EffectGroup& group) {
                            if (group.type != key.type) {
                                return false;
                            }
                            if (key.type == 0) {
                                return group.blockPixels == key.blockPixels;
                            }
                            if (key.type == 1) {
                                return group.gaussianPlan.reductionFactor ==
                                           key.gaussianPlan.reductionFactor &&
                                       group.gaussianPlan.passCount == key.gaussianPlan.passCount &&
                                       group.gaussianPlan.physicalSupportRadius ==
                                           key.gaussianPlan.physicalSupportRadius &&
                                       std::equal(std::begin(group.gaussianPlan.radii),
                                                  std::end(group.gaussianPlan.radii),
                                                  std::begin(key.gaussianPlan.radii));
                            }
                            return true;
                        });
                    if (found == groups.end()) {
                        key.indices.push_back(filterIndex);
                        groups.push_back(std::move(key));
                    } else {
                        found->indices.push_back(filterIndex);
                    }
                    ++g_filterDiagnostics.originalFilterCount;
                }
                std::vector<EffectGroup> spatialGroups;
                for (const EffectGroup& effect : groups) {
                    std::vector<EffectGroup> pending;
                    for (std::uint32_t filterIndex : effect.indices) {
                        EffectGroup subgroup = effect;
                        subgroup.indices = {filterIndex};
                        pending.push_back(std::move(subgroup));
                    }
                    bool merged = false;
                    do {
                        merged = false;
                        for (std::size_t left = 0; left < pending.size() && !merged; ++left) {
                            QRectF leftBounds;
                            for (std::uint32_t filterIndex : pending[left].indices) {
                                leftBounds = leftBounds.isNull()
                                                 ? cachedFilter(filterIndex).logicalBounds
                                                 : leftBounds.united(
                                                       cachedFilter(filterIndex).logicalBounds);
                            }
                            const double physicalOutset =
                                effect.type == 1 ? effect.gaussianPlan.physicalSupportRadius + 0.5
                                                 : 0.5;
                            const double logicalOutset = physicalOutset / devicePixelRatio;
                            const QRectF leftSource = leftBounds.adjusted(
                                -logicalOutset, -logicalOutset, logicalOutset, logicalOutset);
                            for (std::size_t right = left + 1; right < pending.size(); ++right) {
                                QRectF rightBounds;
                                for (std::uint32_t filterIndex : pending[right].indices) {
                                    rightBounds =
                                        rightBounds.isNull()
                                            ? cachedFilter(filterIndex).logicalBounds
                                            : rightBounds.united(
                                                  cachedFilter(filterIndex).logicalBounds);
                                }
                                const QRectF rightSource = rightBounds.adjusted(
                                    -logicalOutset, -logicalOutset, logicalOutset, logicalOutset);
                                if (!leftSource.intersects(rightSource)) {
                                    continue;
                                }
                                pending[left].indices.insert(pending[left].indices.end(),
                                                             pending[right].indices.begin(),
                                                             pending[right].indices.end());
                                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(right));
                                merged = true;
                                break;
                            }
                        }
                    } while (merged);
                    spatialGroups.insert(spatialGroups.end(),
                                         std::make_move_iterator(pending.begin()),
                                         std::make_move_iterator(pending.end()));
                }
                groups = std::move(spatialGroups);
                g_filterDiagnostics.spatialEffectGroupCount += groups.size();
                if (groups.empty()) {
                    continue;
                }
                scenePainter.end();

                // Retain the complete pre-filter surface at this z-order boundary.  The
                // dependency fingerprint includes every lower-z item, so a shape inserted
                // between two filters naturally produces a second cache entry for the same tile.
                if (request.enableFilterTileCache && request.cacheNamespace != nullptr) {
                    if (layerEnd > layerStart + 1) {
                        g_filterDiagnostics.sourceMergedNodes +=
                            static_cast<std::size_t>(layerEnd - layerStart - 1);
                    }
                    const QRect physicalBounds =
                        physicalSurfaceGeometry(surfaceBounds, devicePixelRatio).pixelBounds;
                    snow_canvas_filter_tile_cache::Key sourceKey{
                        request.cacheNamespace,
                        request.filterTileCoordinate,
                        physicalBounds,
                        QSize(qMax(1, qRound(displayInfo.surface_width)),
                             qMax(1, qRound(displayInfo.surface_height))),
                        filterTileDprBits(devicePixelRatio),
                        request.filterTileContentKey,
                        filterDependencyFingerprintForRegion(sceneItems, layerStart, displayInfo,
                                                             surfaceBounds),
                        0,
                    };
                    std::shared_ptr<const snow_canvas_filter_tile_cache::Entry> retainedSource =
                        preloadedLookupAttempted && preloadedLayerStart == layerStart
                            ? preloadedSource
                            : snow_canvas_filter_tile_cache::find(sourceKey);
                    if (retainedSource && retainedSource->image.size() == scene.size() &&
                        retainedSource->image.format() == scene.format()) {
                        for (int row = 0; row < scene.height(); ++row) {
                            std::memcpy(scene.scanLine(row), retainedSource->image.constScanLine(row),
                                        static_cast<std::size_t>(scene.bytesPerLine()));
                        }
                    } else {
                        snow_canvas_filter_tile_cache::store(sourceKey, scene, physicalBounds);
                    }
                }
                ++g_filterDiagnostics.filterLayerCount;
                QImage* preLayerSource = nullptr;
                if (groups.size() > 1) {
                    QImage& pooledPreLayer =
                        workspace.preLayerScratch(scene.size(), devicePixelRatio);
                    if (pooledPreLayer.isNull()) {
                        scenePainter.begin(&scene);
                        scenePainter.setFont(painter.font());
                        scenePainter.setRenderHints(painter.renderHints());
                        scenePainter.translate(-surfaceGeometry.logicalOrigin);
                        continue;
                    }
                    const std::size_t rowBytes =
                        static_cast<std::size_t>(scene.width()) * sizeof(QRgb);
                    for (int y = 0; y < scene.height(); ++y) {
                        std::memcpy(pooledPreLayer.scanLine(y), scene.constScanLine(y), rowBytes);
                    }
                    g_filterDiagnostics.copiedBytes += rowBytes * scene.height();
                    preLayerSource = &pooledPreLayer;
                }
                QRect layerDestination = surfaceBounds;
                const auto plannedDestination = std::find_if(
                    component.layerDestinations.begin(), component.layerDestinations.end(),
                    [layerStart](const auto& entry) { return entry.first == layerStart; });
                if (plannedDestination != component.layerDestinations.end()) {
                    layerDestination = plannedDestination->second;
                }
                for (const EffectGroup& group : groups) {
                    QRectF maskBounds;
                    for (std::uint32_t filterIndex : group.indices) {
                        maskBounds =
                            maskBounds.isNull()
                                ? cachedFilter(filterIndex).logicalBounds
                                : maskBounds.united(cachedFilter(filterIndex).logicalBounds);
                    }
                    maskBounds =
                        maskBounds.intersected(surfaceBounds).intersected(layerDestination);
                    const QRect maskPixels = physicalRectForLogicalBounds(
                        maskBounds, surfaceGeometry.pixelBounds, devicePixelRatio);
                    if (maskPixels.isEmpty()) {
                        continue;
                    }
                    const QImage& source = preLayerSource == nullptr ? scene : *preLayerSource;
                    const QPointF sceneImageOrigin = surfaceGeometry.logicalOrigin;
                    snow_canvas_filter_render::Parameters directParameters =
                        filterParameters(displayInfo, sceneItems[group.indices.front()],
                                         devicePixelRatio, sceneImageOrigin);
                    directParameters.logicalSamplingRadius =
                        sceneItems[group.indices.front()].filter.sampling_radius *
                        snow_canvas_render_geometry::sceneProjection(displayInfo).cameraZoom;
                    if (isColorEffect(group.type)) {
                        // Grayscale and inversion are full-strength effects; opacity controls
                        // coverage.
                        directParameters.strength = 1.0;
                    }
                    bool applied = false;
                    const SnowCanvasSceneItem& onlyFilter = sceneItems[group.indices.front()];
                    bool opaqueRectangles = group.type == 1 || isColorEffect(group.type);
                    QRegion opaqueRegion;
                    for (std::uint32_t filterIndex : group.indices) {
                        const bool directRectangle =
                            cachedFilter(filterIndex).axisAlignedRect &&
                            (group.type == 1 || cachedFilter(filterIndex).devicePixelAlignedRect);
                        opaqueRectangles = opaqueRectangles && directRectangle &&
                                           colorFilterCoverage(sceneItems[filterIndex]) >= 1.0;
                        if (opaqueRectangles) {
                            const QRectF bounds = cachedFilter(filterIndex)
                                                      .logicalBounds.intersected(surfaceBounds)
                                                      .intersected(layerDestination);
                            opaqueRegion += physicalRectForLogicalBounds(
                                bounds, surfaceGeometry.pixelBounds, devicePixelRatio);
                        }
                    }
                    const bool constantRectangle =
                        group.indices.size() == 1 &&
                        (group.type == 1 || isColorEffect(group.type)) &&
                        cachedFilter(group.indices.front()).axisAlignedRect &&
                        (group.type == 1 ||
                         cachedFilter(group.indices.front()).devicePixelAlignedRect);
                    if (opaqueRectangles) {
                        applied = snow_canvas_filter_render::applyRegion(
                            source, scene, opaqueRegion, directParameters, &workspace, execution);
                        if (applied) {
                            ++g_filterDiagnostics.opaqueRectDispatchCount;
                            ++g_filterDiagnostics.denseDispatchCount;
                        }
                    } else if (constantRectangle) {
                        applied = snow_canvas_filter_render::applyRect(
                            source, scene, maskPixels, colorFilterCoverage(onlyFilter),
                            directParameters, &workspace, execution);
                        if (applied) {
                            ++g_filterDiagnostics.constantOpacityRectDispatchCount;
                            ++g_filterDiagnostics.denseDispatchCount;
                        }
                    } else {
                        const bool hasPenFilter =
                            std::any_of(group.indices.begin(), group.indices.end(),
                                        [sceneItems](std::uint32_t index) {
                                            return sceneItems[index].is_free_draw != 0;
                                        });
                        const bool onlyPenFilters =
                            std::all_of(group.indices.begin(), group.indices.end(),
                                        [sceneItems](std::uint32_t index) {
                                            return sceneItems[index].is_free_draw != 0;
                                        });
                        if (onlyPenFilters && group.type != 1) {
                            std::size_t coveredPixels = 0;
                            std::size_t boundingPixels = 0;
                            applied = applyPenTilesDirect(
                                source, scene, maskPixels, surfaceGeometry.pixelBounds,
                                group.indices, sceneItems, displayInfo, devicePixelRatio,
                                penMaskAtlas, directParameters, workspace, execution,
                                &coveredPixels, &boundingPixels);
                            if (applied) {
                                if (execution.forceDenseMask) {
                                    ++g_filterDiagnostics.denseDispatchCount;
                                } else {
                                    ++g_filterDiagnostics.sparseDispatchCount;
                                }
                                g_filterDiagnostics.maskCoveredPixelCount += coveredPixels;
                                g_filterDiagnostics.maskBoundingPixelCount +=
                                    static_cast<std::size_t>(maskPixels.width()) *
                                    static_cast<std::size_t>(maskPixels.height());
                                g_filterDiagnostics.maskPixelCount += boundingPixels;
                            }
                        } else {
                            QImage* generatedMask = nullptr;
                            SparseMaskScan generatedScan;
                            const StageTimer maskTimer{
                                g_filterDiagnostics.maskConstructionNanoseconds};
                            QImage& mask =
                                workspace.alphaScratch(maskPixels.size(), devicePixelRatio);
                            if (mask.isNull()) {
                                continue;
                            }
                            generatedMask = &mask;
                            mask.fill(0);
                            QPainter maskPainter(&mask);
                            maskPainter.setRenderHint(QPainter::Antialiasing, true);
                            const QPointF maskLogicalOrigin =
                                surfaceGeometry.logicalOrigin +
                                QPointF(maskPixels.left() / devicePixelRatio,
                                        maskPixels.top() / devicePixelRatio);
                            maskPainter.translate(-maskLogicalOrigin);
                            maskPainter.setClipRect(maskBounds);
                            for (std::uint32_t filterIndex : group.indices) {
                                const SnowCanvasSceneItem& filter = sceneItems[filterIndex];
                                if (filter.is_free_draw != 0) {
                                    continue;
                                }
                                maskPainter.setOpacity(colorFilterCoverage(filter));
                                const QPainterPath& maskPath = filterPath(filterIndex);
                                maskPainter.fillPath(maskPath, Qt::white);
                            }
                            maskPainter.end();
                            if (hasPenFilter) {
                                try {
                                    generatedScan = compositePenTilesIntoMask(
                                        mask, maskPixels, surfaceGeometry.pixelBounds,
                                        group.indices, sceneItems, displayInfo,
                                        devicePixelRatio, penMaskAtlas, execution,
                                        onlyPenFilters);
                                } catch (const std::bad_alloc&) {
                                    continue;
                                }
                            }
                            const std::size_t effectPixels =
                                static_cast<std::size_t>(maskPixels.width()) *
                                static_cast<std::size_t>(maskPixels.height());
                            bool scanSucceeded = true;
                            if (!onlyPenFilters) {
                                const StageTimer scanTimer{
                                    g_filterDiagnostics.maskScanNanoseconds};
                                try {
                                    generatedScan =
                                        scanSparseMask(*generatedMask, maskPixels.topLeft());
                                } catch (const std::bad_alloc&) {
                                    scanSucceeded = false;
                                }
                            }
                            const QImage& maskImage = *generatedMask;
                            const auto& spans = generatedScan.spans;
                            const auto& occupiedBlocks = generatedScan.occupiedBlocks;
                            const std::size_t coveredPixels = generatedScan.coveredPixels;
                            g_filterDiagnostics.maskBoundingPixelCount += effectPixels;
                            g_filterDiagnostics.maskCoveredPixelCount += coveredPixels;
                            const bool useSparse = scanSucceeded && !execution.forceDenseMask &&
                                                   effectPixels >= 16'384 &&
                                                   coveredPixels * 2 <= effectPixels;
                            if (useSparse) {
                                applied = snow_canvas_filter_render::applyMaskedSparse(
                                    source, scene, maskImage, maskPixels.topLeft(), maskPixels, spans,
                                    occupiedBlocks, directParameters, &workspace, execution);
                                if (applied) {
                                    ++g_filterDiagnostics.sparseDispatchCount;
                                } else {
                                    applied = snow_canvas_filter_render::applyMasked(
                                        source, scene, maskImage, maskPixels.topLeft(), maskPixels,
                                        directParameters, &workspace, execution);
                                    if (applied) {
                                        ++g_filterDiagnostics.denseDispatchCount;
                                    }
                                }
                            } else {
                                applied = snow_canvas_filter_render::applyMasked(
                                    source, scene, maskImage, maskPixels.topLeft(), maskPixels,
                                    directParameters, &workspace, execution);
                                if (applied) {
                                    ++g_filterDiagnostics.denseDispatchCount;
                                }
                            }
                            g_filterDiagnostics.maskPixelCount += effectPixels;
                        }
                    }
                    if (!applied) {
                        continue;
                    }
                    ++g_filterDiagnostics.effectDispatchCount;
                    g_filterDiagnostics.batchedFilterCount += group.indices.size() - 1;
                    const std::size_t effectPixels = static_cast<std::size_t>(maskPixels.width()) *
                                                     static_cast<std::size_t>(maskPixels.height());
                    g_filterDiagnostics.peakEffectPixelCount =
                        std::max(g_filterDiagnostics.peakEffectPixelCount, effectPixels);
                }
                scenePainter.begin(&scene);
                scenePainter.setFont(painter.font());
                scenePainter.setRenderHints(painter.renderHints());
                scenePainter.translate(-surfaceGeometry.logicalOrigin);
            }
            scenePainter.end();
            const StageTimer presentationTimer{g_filterDiagnostics.presentationNanoseconds};
            painter.save();
            painter.setClipRegion(exposedRegion.intersected(QRegion(surfaceBounds)));
            painter.drawImage(surfaceGeometry.logicalOrigin, scene);
            painter.restore();
        }
        workspace.finishFrame();
        const auto& kernelDiagnostics = workspace.diagnostics();
        g_filterDiagnostics.allocatedBytes += kernelDiagnostics.allocatedBytes;
        g_filterDiagnostics.copiedBytes += kernelDiagnostics.copiedBytes;
        g_filterDiagnostics.scratchReuseCount = kernelDiagnostics.scratchReuseCount;
        g_filterDiagnostics.parallelJobs = kernelDiagnostics.parallelJobs;
        g_filterDiagnostics.retainedWorkspaceBytes = kernelDiagnostics.retainedBytes;
        g_filterDiagnostics.gaussianPasses = kernelDiagnostics.gaussianPasses;
        g_filterDiagnostics.gaussianDownsampleAvx2Executions =
            kernelDiagnostics.gaussianDownsampleAvx2Executions;
        g_filterDiagnostics.gaussianReconstructionAvx2Executions =
            kernelDiagnostics.gaussianReconstructionAvx2Executions;
        g_filterDiagnostics.downsampleNanoseconds = kernelDiagnostics.downsampleNanoseconds;
        g_filterDiagnostics.reducedBlurNanoseconds = kernelDiagnostics.reducedBlurNanoseconds;
        g_filterDiagnostics.reconstructionNanoseconds = kernelDiagnostics.reconstructionNanoseconds;
        g_filterDiagnostics.simdBackend =
            snow_canvas_filter_render::simdBackendName(kernelDiagnostics.backend);
        const snow_canvas_pen_mask::Diagnostics atlasDiagnostics =
            hasPenFilterItems ? penMaskAtlas.takeDiagnostics()
                              : snow_canvas_pen_mask::Diagnostics{};
        g_filterDiagnostics.penQueriedChunkCount += atlasDiagnostics.queriedChunks;
        g_filterDiagnostics.penCulledChunkCount += atlasDiagnostics.culledChunks;
        g_filterDiagnostics.penRasterizedTileCount += atlasDiagnostics.rasterizedTiles;
        g_filterDiagnostics.penRasterizedPixelCount += atlasDiagnostics.rasterizedPixels;
        g_filterDiagnostics.penAtlasHits += atlasDiagnostics.hits;
        g_filterDiagnostics.penAtlasMisses += atlasDiagnostics.misses;
        g_filterDiagnostics.penAtlasEvictions += atlasDiagnostics.evictions;
        g_filterDiagnostics.penAtlasReusedAfterPatch += atlasDiagnostics.reusedAfterPatch;
        g_filterDiagnostics.penSimdRasterExecutions += atlasDiagnostics.simdRasterExecutions;
        g_filterDiagnostics.retainedPenAtlasBytes = atlasDiagnostics.retainedBytes;
        g_filterDiagnostics.workingSurfacePixelCount = g_filterDiagnostics.totalWorkingPixelCount;
        g_filterDiagnostics.layerCount = g_filterDiagnostics.filterLayerCount;
        g_filterDiagnostics.filterPassCount = g_filterDiagnostics.effectDispatchCount;
        return;
    }
    if (backgroundImage != nullptr && !backgroundImage->isNull()) {
        painter.drawImage(QRectF(0.0, 0.0, displayInfo.surface_width, displayInfo.surface_height),
                          *backgroundImage);
    }
    if (backgroundRenderer != nullptr && backgroundContext != nullptr) {
        painter.save();
        backgroundRenderer->renderBeforeCanvas(painter, *backgroundContext);
        painter.restore();
    }
    if (candidateIndices != nullptr) {
        for (std::uint32_t candidate = 0; candidate < candidateCount; ++candidate) {
            const std::uint32_t index = candidateIndices[candidate];
            if (index < sceneItemCount) {
                drawSceneItem(painter, displayInfo, sceneItems[index]);
            }
        }
        return;
    }
    for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
        const SnowCanvasSceneItem& item = sceneItems[index];
        if (!exposedRegion.intersects(alignedRectForBounds(sceneItemBounds(displayInfo, item)))) {
            continue;
        }
        drawSceneItem(painter, displayInfo, item);
    }
}
} // namespace

FilterRenderDiagnostics filterRenderDiagnosticsForCurrentThread() {
    return g_filterDiagnostics;
}

void resetFilterRenderDiagnosticsForCurrentThread() {
    g_filterDiagnostics = {};
}

void accumulateFilterRenderDiagnostics(FilterRenderDiagnostics& target,
                                       const FilterRenderDiagnostics& source) {
    target.usedFilterPath = target.usedFilterPath || source.usedFilterPath;
    target.exposedPixelCount += source.exposedPixelCount;
    target.totalWorkingPixelCount += source.totalWorkingPixelCount;
    target.peakWorkingPixelCount =
        std::max(target.peakWorkingPixelCount, source.peakWorkingPixelCount);
    target.surfaceComponentCount += source.surfaceComponentCount;
    target.spatialCandidateCount += source.spatialCandidateCount;
    target.replayedItemCount += source.replayedItemCount;
    target.filterLayerCount += source.filterLayerCount;
    target.originalFilterCount += source.originalFilterCount;
    target.effectDispatchCount += source.effectDispatchCount;
    target.batchedFilterCount += source.batchedFilterCount;
    target.maskPixelCount += source.maskPixelCount;
    target.maskBoundingPixelCount += source.maskBoundingPixelCount;
    target.maskCoveredPixelCount += source.maskCoveredPixelCount;
    target.sparseDispatchCount += source.sparseDispatchCount;
    target.denseDispatchCount += source.denseDispatchCount;
    target.spatialEffectGroupCount += source.spatialEffectGroupCount;
    target.penGeometryChunkBuildCount += source.penGeometryChunkBuildCount;
    target.penGeometryChunkReuseCount += source.penGeometryChunkReuseCount;
    target.penQueriedChunkCount += source.penQueriedChunkCount;
    target.penCulledChunkCount += source.penCulledChunkCount;
    target.penRasterizedTileCount += source.penRasterizedTileCount;
    target.penRasterizedPixelCount += source.penRasterizedPixelCount;
    target.penAtlasHits += source.penAtlasHits;
    target.penAtlasMisses += source.penAtlasMisses;
    target.penAtlasEvictions += source.penAtlasEvictions;
    target.penAtlasReusedAfterPatch += source.penAtlasReusedAfterPatch;
    target.penSimdRasterExecutions += source.penSimdRasterExecutions;
    target.retainedPenAtlasBytes =
        std::max(target.retainedPenAtlasBytes, source.retainedPenAtlasBytes);
    target.allocatedBytes += source.allocatedBytes;
    target.copiedBytes += source.copiedBytes;
    target.scratchReuseCount += source.scratchReuseCount;
    target.sourceTileHits += source.sourceTileHits;
    target.sourceTileMisses += source.sourceTileMisses;
    target.sourceTileEvictions += source.sourceTileEvictions;
    target.sourceTileCandidates += source.sourceTileCandidates;
    target.sourceTileVisits += source.sourceTileVisits;
    target.sourceDependencyInvalidations += source.sourceDependencyInvalidations;
    target.sourceMergedNodes += source.sourceMergedNodes;
    target.sourceOverlappingNodes += source.sourceOverlappingNodes;
    target.retainedSourceBytes = std::max(target.retainedSourceBytes, source.retainedSourceBytes);
    target.parallelJobs += source.parallelJobs;
    target.retainedWorkspaceBytes =
        std::max(target.retainedWorkspaceBytes, source.retainedWorkspaceBytes);
    target.gaussianPasses += source.gaussianPasses;
    target.gaussianDownsampleAvx2Executions += source.gaussianDownsampleAvx2Executions;
    target.gaussianReconstructionAvx2Executions += source.gaussianReconstructionAvx2Executions;
    target.opaqueRectDispatchCount += source.opaqueRectDispatchCount;
    target.constantOpacityRectDispatchCount += source.constantOpacityRectDispatchCount;
    target.sceneReplayNanoseconds += source.sceneReplayNanoseconds;
    target.maskConstructionNanoseconds += source.maskConstructionNanoseconds;
    target.pathConstructionNanoseconds += source.pathConstructionNanoseconds;
    target.maskScanNanoseconds += source.maskScanNanoseconds;
    target.downsampleNanoseconds += source.downsampleNanoseconds;
    target.reducedBlurNanoseconds += source.reducedBlurNanoseconds;
    target.reconstructionNanoseconds += source.reconstructionNanoseconds;
    target.presentationNanoseconds += source.presentationNanoseconds;
    if (source.simdBackend != nullptr &&
        std::string_view(source.simdBackend) == std::string_view("avx2")) {
        target.simdBackend = "avx2";
    }
    target.workingSurfacePixelCount += source.workingSurfacePixelCount;
    target.peakEffectPixelCount =
        std::max(target.peakEffectPixelCount, source.peakEffectPixelCount);
    target.recorderCount += source.recorderCount;
    target.layerCount += source.layerCount;
    target.filterPassCount += source.filterPassCount;
}

void drawOverlayItem(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                     const SnowCanvasOverlayItem& item) {
    switch (item.kind) {
    case SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT:
        drawOverlayRectItem(painter, displayInfo, item);
        break;
    case SNOW_OVERLAY_DISPLAY_ITEM_SNAP_GUIDE:
        drawSnapGuide(painter, displayInfo, item);
        break;
    case SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION:
        drawFocusConnectionItem(painter, displayInfo, item);
        break;
    case SNOW_OVERLAY_DISPLAY_ITEM_PEN_FILTER_CONTOUR:
        drawPenFilterContourItem(painter, displayInfo, item);
        break;
    }
}

std::size_t hatchTextureCacheEntryCountForCurrentThread() {
    return snow_canvas_fill_render::hatchTextureCacheEntryCountForCurrentThread();
}

void renderOverlayItems(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                        const SnowCanvasOverlayItem* overlayItems, std::uint32_t overlayItemCount,
                        const QRegion& exposedRegion, const SceneDisplayInfo* sceneDisplayInfo,
                        const SnowCanvasSceneItem* sceneItems, std::uint32_t sceneItemCount) {
    if (overlayItems == nullptr) {
        return;
    }

    for (std::uint32_t index = 0; index < overlayItemCount; ++index) {
        const SnowCanvasOverlayItem& item = overlayItems[index];
        if (!exposedRegion.intersects(alignedRectForBounds(overlayItemBounds(displayInfo, item)))) {
            continue;
        }
        if (drawTextHoverOverlay(painter, sceneDisplayInfo, sceneItems, sceneItemCount, item)) {
            continue;
        }
        drawOverlayItem(painter, displayInfo, item);
    }
}

void renderOverlayItems(QPainter& painter, const OverlayDisplayInfo& displayInfo,
                        const SnowOverlayDisplayItem* overlayItems, std::uint32_t overlayItemCount,
                        const QRegion& exposedRegion) {
    if (overlayItems == nullptr) {
        return;
    }
    for (std::uint32_t index = 0; index < overlayItemCount; ++index) {
        const SnowOverlayDisplayItem& item = overlayItems[index];
        if (!exposedRegion.intersects(alignedRectForBounds(overlayItemBounds(displayInfo, item)))) {
            continue;
        }
        drawOverlayItem(painter, displayInfo, item);
    }
}

void drawDirtyRectOverlay(QPainter& painter, const SnowDirtyRect* dirtyRects,
                          std::uint32_t dirtyRectCount, const QColor& stroke, const QColor& fill,
                          int paddingPx) {
    painter.save();
    QPen pen(stroke, 1.5);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(fill);
    if (dirtyRects == nullptr) {
        painter.restore();
        return;
    }
    for (std::uint32_t index = 0; index < dirtyRectCount; ++index) {
        const SnowDirtyRect& dirtyRect = dirtyRects[index];
        const QRect drawRect = snow_canvas_display::dirtyRectToUpdateRect(dirtyRect, paddingPx);
        if (!drawRect.isEmpty()) {
            painter.drawRect(drawRect.adjusted(0, 0, -1, -1));
        }
    }
    painter.restore();
}

} // namespace snow_canvas_renderer
