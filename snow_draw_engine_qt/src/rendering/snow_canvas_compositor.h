#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_canvas_renderer.h"

#include <QPointF>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QRegion>

class QPainter;
class QWidget;
class SnowCanvasCustomRenderer;
struct SnowCanvasRenderContext;

namespace snow_canvas_compositor {

struct Frame {
    const SnowCanvasDisplayCache* displayCache = nullptr;
    const SceneDisplayInfo* sceneInfo = nullptr;
    const WatermarkDisplayInfo* watermarkInfo = nullptr;
    const SpotlightDisplayInfo* spotlightInfo = nullptr;
    const SnowSpotlightCutout* spotlightCutouts = nullptr;
    std::uint32_t spotlightCutoutCount = 0;
    const OverlayDisplayInfo* overlayInfo = nullptr;
    const SnowCanvasSceneItem* sceneItems = nullptr;
    std::uint32_t sceneItemCount = 0;
    const SnowCanvasOverlayItem* overlayItems = nullptr;
    std::uint32_t overlayItemCount = 0;
    const SnowDirtyRect* sceneDirtyRects = nullptr;
    std::uint32_t sceneDirtyRectCount = 0;
    const SnowDirtyRect* decorationDirtyRects = nullptr;
    std::uint32_t decorationDirtyRectCount = 0;
    const SnowDirtyRect* overlayDirtyRects = nullptr;
    std::uint32_t overlayDirtyRectCount = 0;
    const QImage* backgroundImage = nullptr;
    SnowCanvasCustomRenderer* backgroundRenderer = nullptr;
    const SnowCanvasRenderContext* backgroundContext = nullptr;
    snow_canvas_filter_render::RenderWorkspace* workspace = nullptr;
    snow_canvas_pen_mask::PenMaskAtlas* penMaskAtlas = nullptr;
    const QWidget* widget = nullptr;
    QRectF watermarkRenderArea;
    bool hasWatermarkRenderArea = false;
    QRectF spotlightRenderArea;
    bool hasSpotlightRenderArea = false;
    bool clearBackgroundEnabled = true;
    bool showDirtyRects = false;
};

void clearSurface(QPainter& painter, const Frame& frame);
void renderSceneContent(QPainter& painter, const Frame& frame);
void renderDocumentDecorations(QPainter& painter, const Frame& frame);
void renderEditorOverlays(QPainter& painter, const Frame& frame);
// Retained names for custom renderer and test callers during the pass split.
void renderCanvasContent(QPainter& painter, const Frame& frame);
void renderOverlays(QPainter& painter, const Frame& frame);
void renderContent(QPainter& painter, const Frame& frame);

} // namespace snow_canvas_compositor
