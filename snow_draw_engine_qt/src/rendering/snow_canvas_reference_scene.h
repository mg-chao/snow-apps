#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_viewport.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

#include <QFont>
#include <QPainter>

// Widget-owned, single-resolution surface. The fixed viewport preserves the full
// source scene and its explicit filter passes even when the live viewport crops it.
class SnowCanvasReferenceScene final {
  public:
    bool render(SnowRuntime runtime, const SnowCanvasFilterRenderReference& reference,
                const snow_canvas_renderer::SceneRenderRequest& request,
                const QTransform& canvasToView);
    void clearRenderState();
    void reset();

  private:
    SnowCanvasViewport m_viewport;
    SnowCanvasDisplayCache m_displayCache;
    SnowCanvasFilterRenderReference m_reference;
    QImage m_image;
    std::uint64_t m_sceneRevision = 0;
    std::uint64_t m_backgroundRevision = 0;
    SnowCanvasCustomRenderer* m_backgroundRenderer = nullptr;
    QFont m_font;
    QPainter::RenderHints m_renderHints;
    bool m_clearBackgroundEnabled = true;
};
