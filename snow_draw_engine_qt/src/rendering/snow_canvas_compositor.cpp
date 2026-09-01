#include "snow_canvas_compositor.h"
#include "snow_canvas_spotlight_renderer.h"

#include <QPainter>
#include <QWidget>

#include <vector>

namespace snow_canvas_compositor {
namespace {

void renderDirtyRectOverlay(QPainter& painter, const Frame& frame) {
    if (!frame.showDirtyRects) {
        return;
    }

    snow_canvas_renderer::drawDirtyRectOverlay(painter, frame.sceneDirtyRects,
                                               frame.sceneDirtyRectCount, QColor(255, 149, 0, 220),
                                               QColor(255, 149, 0, 48));
    snow_canvas_renderer::drawDirtyRectOverlay(painter, frame.decorationDirtyRects,
                                               frame.decorationDirtyRectCount,
                                               QColor(170, 0, 255, 220), QColor(170, 0, 255, 36));
    snow_canvas_renderer::drawDirtyRectOverlay(painter, frame.overlayDirtyRects,
                                               frame.overlayDirtyRectCount,
                                               QColor(0, 170, 255, 220), QColor(0, 170, 255, 40));
}

} // namespace

void clearSurface(QPainter& painter, const Frame& frame) {
    if (frame.sceneInfo == nullptr || frame.widget == nullptr) {
        return;
    }

    const QRegion exposedRegion = painter.clipRegion();
    const QRect exposedRect = exposedRegion.boundingRect().intersected(frame.widget->rect());
    if (exposedRect.isEmpty()) {
        return;
    }

    if (frame.clearBackgroundEnabled) {
        painter.fillRect(exposedRect, snow_canvas_renderer::toQColor(frame.sceneInfo->clear_color));
    }
}

void renderDocumentDecorations(QPainter& painter, const Frame& frame) {
    if (frame.widget == nullptr) {
        return;
    }
    const QRegion exposedRegion = painter.clipRegion();
    if (frame.sceneInfo != nullptr && frame.spotlightInfo != nullptr) {
        const QRectF spotlightArea =
            frame.hasSpotlightRenderArea
                ? frame.spotlightRenderArea
                : QRectF(0.0, 0.0, frame.sceneInfo->surface_width, frame.sceneInfo->surface_height);
        snow_canvas_spotlight_renderer::render(
            painter, *frame.sceneInfo, *frame.spotlightInfo, frame.spotlightCutouts,
            frame.spotlightCutoutCount, spotlightArea, exposedRegion);
    }
    if (frame.watermarkInfo == nullptr) {
        return;
    }
    const QRectF renderArea = frame.hasWatermarkRenderArea
                                  ? frame.watermarkRenderArea
                                  : QRectF(0.0, 0.0, frame.watermarkInfo->surface_width,
                                           frame.watermarkInfo->surface_height);
    snow_canvas_renderer::WatermarkPatternRenderer::render(
        painter, snow_canvas_renderer::WatermarkRenderRequest{
                     *frame.watermarkInfo,
                     QRectF(0.0, 0.0, frame.watermarkInfo->surface_width,
                            frame.watermarkInfo->surface_height),
                     renderArea,
                     exposedRegion,
                     painter.deviceTransform(),
                     snow_canvas_renderer::WatermarkRenderPurpose::Widget,
                 });
}

void renderEditorOverlays(QPainter& painter, const Frame& frame) {
    if (frame.sceneInfo == nullptr || frame.overlayInfo == nullptr || frame.widget == nullptr) {
        return;
    }
    const QRegion exposedRegion = painter.clipRegion();
    snow_canvas_renderer::renderOverlayItems(painter, *frame.overlayInfo, frame.overlayItems,
                                             frame.overlayItemCount, exposedRegion, frame.sceneInfo,
                                             frame.sceneItems, frame.sceneItemCount);
    renderDirtyRectOverlay(painter, frame);
}

} // namespace snow_canvas_compositor
