#include "snow_canvas_widget_paint_frame.h"

namespace snow_canvas_widget_paint_frame {

snow_canvas_compositor::Frame build(const Request& request) {
    snow_canvas_compositor::Frame frame;
    frame.widget = request.widget;
    frame.showDirtyRects = request.showDirtyRects;
    frame.workspace = request.workspace;

    if (request.displayCache != nullptr) {
        const SnowCanvasDisplayCache& cache = *request.displayCache;
        frame.displayCache = &cache;
        frame.sceneInfo = &cache.sceneInfo();
        frame.watermarkInfo = &cache.watermarkInfo();
        frame.spotlightInfo = &cache.spotlightInfo();
        frame.spotlightCutouts = cache.spotlightCutouts();
        frame.spotlightCutoutCount = cache.spotlightCutoutCount();
        frame.overlayInfo = &cache.overlayInfo();
        frame.sceneItems = cache.sceneItems();
        frame.sceneItemCount = cache.sceneItemCount();
        frame.overlayItems = cache.overlayItems();
        frame.overlayItemCount = cache.overlayItemCount();
        frame.sceneDirtyRects = cache.sceneDirtyRects();
        frame.sceneDirtyRectCount = cache.sceneDirtyRectCount();
        frame.decorationDirtyRects = cache.decorationDirtyRects();
        frame.decorationDirtyRectCount = cache.decorationDirtyRectCount();
        frame.overlayDirtyRects = cache.overlayDirtyRects();
        frame.overlayDirtyRectCount = cache.overlayDirtyRectCount();
    }

    frame.clearBackgroundEnabled = request.clearBackgroundEnabled;

    return frame;
}

} // namespace snow_canvas_widget_paint_frame
