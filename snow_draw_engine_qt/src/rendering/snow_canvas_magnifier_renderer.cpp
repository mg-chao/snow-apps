#include "snow_canvas_magnifier_renderer.h"

#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"

#include <QPainter>
#include <QTransform>

#include <cmath>

namespace snow_canvas_magnifier_renderer {
namespace {

thread_local RenderDiagnostics g_magnifierDiagnostics;

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) && std::isfinite(rect.width()) &&
           std::isfinite(rect.height());
}

} // namespace

RenderDiagnostics diagnosticsForCurrentThread() {
    return g_magnifierDiagnostics;
}

void resetDiagnosticsForCurrentThread() {
    g_magnifierDiagnostics = {};
}

QPainterPath lensPath(const SnowSceneDisplayItem& item) {
    QPainterPath path;
    const QRectF rect(-item.width / 2.0, -item.height / 2.0, item.width, item.height);
    if (!finiteRect(rect) || !rect.isValid() || !std::isfinite(item.rotation) ||
        !std::isfinite(item.center_x) || !std::isfinite(item.center_y))
        return path;
    if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
        path.addEllipse(rect);
    } else if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
        path.moveTo(0.0, rect.top());
        path.lineTo(rect.right(), 0.0);
        path.lineTo(0.0, rect.bottom());
        path.lineTo(rect.left(), 0.0);
        path.closeSubpath();
    } else {
        path = snow_canvas_render_geometry::roundedRectPath(
            rect, snow_canvas_render_geometry::toViewCornerRadii(item.corner_radii, 1.0, rect));
    }
    QTransform element;
    element.translate(item.center_x, item.center_y);
    element.rotateRadians(item.rotation);
    return element.map(path);
}

void render(QPainter& painter, const SceneDisplayInfo& displayInfo,
            const SnowSceneDisplayItem& item, const QList<SnowCanvasBaseImageSource>* sources) {
    const double zoom = displayInfo.camera_zoom;
    if (!std::isfinite(zoom) || zoom <= 0.0)
        return;
    const QPainterPath canvasPath = lensPath(item);
    if (canvasPath.isEmpty())
        return;
    const QTransform canvasToView(
        zoom, 0.0, 0.0, zoom, displayInfo.surface_width / 2.0 - displayInfo.camera_center_x * zoom,
        displayInfo.surface_height / 2.0 - displayInfo.camera_center_y * zoom);
    const QPainterPath viewPath = canvasToView.map(canvasPath);
    const QRectF exposed = painter.hasClipping() ? painter.clipBoundingRect()
                                                 : QRectF(0.0, 0.0, displayInfo.surface_width,
                                                          displayInfo.surface_height);
    const QRectF visible = viewPath.boundingRect().intersected(exposed);
    const double strokeWidth = item.stroke_width * zoom;
    const bool hasStroke = item.stroke.a != 0 && std::isfinite(strokeWidth) && strokeWidth > 0.0;
    // Source sampling needs the interior; the border can be exposed outside it.
    if (visible.isEmpty() && !hasStroke)
        return;
    ++g_magnifierDiagnostics.renderedMagnifierCount;
    painter.save();
    painter.setOpacity(qBound(0.0, item.opacity, 1.0));
    const auto& geometry = item.magnifier;
    const double factor = geometry.magnification_factor;
    if (!visible.isEmpty() && sources != nullptr && std::isfinite(factor) && factor >= 1.0 &&
        factor <= 10.0 && std::isfinite(geometry.source_center_x) &&
        std::isfinite(geometry.source_center_y)) {
        const double scale = factor * zoom;
        const QPointF destinationCenter = canvasToView.map(QPointF(item.center_x, item.center_y));
        const QTransform sourceToView(scale, 0.0, 0.0, scale,
                                      destinationCenter.x() - geometry.source_center_x * scale,
                                      destinationCenter.y() - geometry.source_center_y * scale);
        const QRectF needed = sourceToView.inverted().mapRect(visible);
        painter.save();
        painter.setClipPath(viewPath, Qt::IntersectClip);
        painter.setWorldTransform(sourceToView, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const auto& source : *sources) {
            if (source.image.isNull() || !finiteRect(source.canvasRect) ||
                !source.canvasRect.isValid() || !finiteRect(source.coverage))
                continue;
            const QRectF coverage = source.coverage.isEmpty()
                                        ? source.canvasRect
                                        : source.canvasRect.intersected(source.coverage);
            const QRectF drawRect = needed.intersected(coverage);
            if (drawRect.isEmpty())
                continue;
            const double pixelsX = source.image.width() / source.canvasRect.width();
            const double pixelsY = source.image.height() / source.canvasRect.height();
            const QRectF pixels((drawRect.x() - source.canvasRect.x()) * pixelsX,
                                (drawRect.y() - source.canvasRect.y()) * pixelsY,
                                drawRect.width() * pixelsX, drawRect.height() * pixelsY);
            // QPainter reads shared source storage directly. Restrict both rectangles to
            // exposed lens pixels instead of allocating a crop or enlarging an image.
            painter.drawImage(drawRect, source.image, pixels);
            ++g_magnifierDiagnostics.sourceLayerDrawCount;
        }
        painter.restore();
    }
    if (hasStroke) {
        painter.setPen(QPen(snow_canvas_renderer::toQColor(item.stroke), strokeWidth, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(viewPath);
    }
    painter.restore();
}

} // namespace snow_canvas_magnifier_renderer
