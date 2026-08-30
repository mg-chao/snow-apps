#include "snow_canvas_spotlight_renderer.h"

#include "snow_canvas_renderer.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <vector>

namespace snow_canvas_spotlight_renderer {
namespace {

struct ValidCutout {
    SnowSpotlightCutout value{};
    QRectF viewBounds;
};

thread_local RenderDiagnostics g_diagnostics;

std::size_t regionPixels(const QRegion& region) {
    std::size_t pixels = 0;
    for (const QRect& rect : region) {
        pixels += static_cast<std::size_t>(rect.width()) * static_cast<std::size_t>(rect.height());
    }
    return pixels;
}

QTransform canvasToViewTransform(const SceneDisplayInfo& sceneInfo) {
    const qreal zoom = sceneInfo.camera_zoom > 0.0 ? sceneInfo.camera_zoom : 1.0;
    return QTransform(zoom, 0.0, 0.0, zoom,
                      sceneInfo.surface_width / 2.0 - sceneInfo.camera_center_x * zoom,
                      sceneInfo.surface_height / 2.0 - sceneInfo.camera_center_y * zoom);
}

QPainterPath transformedCutoutPath(const SnowSpotlightCutout& cutout,
                                   const QTransform& canvasToView) {
    QPainterPath rectangle;
    rectangle.addRect(
        QRectF(-cutout.width / 2.0, -cutout.height / 2.0, cutout.width, cutout.height));
    QTransform element;
    element.translate(cutout.center_x, cutout.center_y);
    element.rotateRadians(cutout.rotation);
    return canvasToView.map(element.map(rectangle));
}

bool validCutout(const SnowSpotlightCutout& cutout) {
    return std::isfinite(cutout.center_x) && std::isfinite(cutout.center_y) &&
           std::isfinite(cutout.width) && std::isfinite(cutout.height) &&
           std::isfinite(cutout.rotation) && cutout.width > 0.0 && cutout.height > 0.0;
}

std::vector<ValidCutout> visibleCutouts(const SnowSpotlightCutout* cutouts,
                                        std::uint32_t cutoutCount, const QTransform& canvasToView,
                                        const QRectF& paintBounds) {
    std::vector<ValidCutout> visible;
    if (cutouts == nullptr || cutoutCount == 0) {
        return visible;
    }
    visible.reserve(cutoutCount);
    for (std::uint32_t index = 0; index < cutoutCount; ++index) {
        const SnowSpotlightCutout& cutout = cutouts[index];
        if (!validCutout(cutout)) {
            ++g_diagnostics.locallyCulledCutoutCount;
            continue;
        }
        const QRectF localRect(-cutout.width / 2.0, -cutout.height / 2.0, cutout.width,
                               cutout.height);
        QTransform element;
        element.translate(cutout.center_x, cutout.center_y);
        element.rotateRadians(cutout.rotation);
        const QRectF bounds = canvasToView.mapRect(element.mapRect(localRect));
        if (!bounds.isValid() || !bounds.intersects(paintBounds)) {
            ++g_diagnostics.locallyCulledCutoutCount;
            continue;
        }
        visible.push_back(ValidCutout{cutout, bounds});
    }
    return visible;
}

void fillDirect(QPainter& painter, const QColor& baseColor, float opacity, const QRectF& renderArea,
                const QRegion& paintRegion, const std::vector<ValidCutout>& visible,
                const QTransform& canvasToView) {
    QColor effectiveColor = baseColor;
    effectiveColor.setAlphaF(baseColor.alphaF() * std::clamp(opacity, 0.0F, 1.0F));
    painter.save();
    painter.setClipRegion(paintRegion, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (visible.empty()) {
        painter.fillRect(renderArea, effectiveColor);
        painter.restore();
        return;
    }

    QPainterPath holes;
    holes.setFillRule(Qt::WindingFill);
    for (const ValidCutout& cutout : visible) {
        holes.addPath(transformedCutoutPath(cutout.value, canvasToView));
        ++g_diagnostics.processedCutoutCount;
    }
    QPainterPath coverage;
    coverage.addRect(renderArea);
    painter.fillPath(coverage.subtracted(holes), effectiveColor);
    painter.restore();
}

} // namespace

RenderDiagnostics diagnosticsForCurrentThread() {
    return g_diagnostics;
}

void resetDiagnosticsForCurrentThread() {
    g_diagnostics = RenderDiagnostics{};
}

void render(QPainter& painter, const SceneDisplayInfo& sceneInfo,
            const SpotlightDisplayInfo& spotlightInfo, const SnowSpotlightCutout* cutouts,
            std::uint32_t cutoutCount, const QRectF& renderArea, const QRegion& exposedRegion) {
    const QColor baseColor = snow_canvas_renderer::toQColor(spotlightInfo.color);
    const float opacity = static_cast<float>(std::clamp(spotlightInfo.opacity, 0.0, 1.0));
    const QRectF normalizedArea = renderArea.normalized();
    if (!spotlightInfo.active || baseColor.alphaF() <= 0.0 || opacity <= 0.0 ||
        !normalizedArea.isValid() || normalizedArea.isEmpty() || exposedRegion.isEmpty()) {
        ++g_diagnostics.earlyExitCount;
        return;
    }

    const QRegion paintRegion = exposedRegion.intersected(QRegion(normalizedArea.toAlignedRect()));
    if (paintRegion.isEmpty()) {
        ++g_diagnostics.earlyExitCount;
        return;
    }

    const QTransform canvasToView = canvasToViewTransform(sceneInfo);
    const std::vector<ValidCutout> visible = visibleCutouts(
        cutouts, cutoutCount, canvasToView, normalizedArea.intersected(paintRegion.boundingRect()));
    g_diagnostics.renderedPixelCount += regionPixels(paintRegion);
    g_diagnostics.renderedRegionCount += static_cast<std::size_t>(paintRegion.rectCount());

    if (visible.empty()) {
        ++g_diagnostics.zeroCutoutFastPathCount;
        fillDirect(painter, baseColor, opacity, normalizedArea, paintRegion, visible, canvasToView);
        return;
    }

    fillDirect(painter, baseColor, opacity, normalizedArea, paintRegion, visible, canvasToView);
}

} // namespace snow_canvas_spotlight_renderer
