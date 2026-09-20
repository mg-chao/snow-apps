#pragma once

#include "snow_canvas_compositor.h"
#include "snow_canvas_display_cache.h"

class QWidget;

namespace snow_canvas_widget_paint_frame {

struct Request {
    const SnowCanvasDisplayCache* displayCache = nullptr;
    QRect viewportRect;
    bool clearBackgroundEnabled = true;
    bool showDirtyRects = false;
    snow_canvas_filter_render::RenderWorkspace* workspace = nullptr;
};

snow_canvas_compositor::Frame build(const Request& request);

} // namespace snow_canvas_widget_paint_frame
