#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

std::uint64_t SnowCanvasCustomRenderer::contentRevision() const {
    return 0;
}

void SnowCanvasCustomRenderer::clearRenderState() {}

void SnowCanvasCustomRenderer::renderBeforeCanvas(QPainter& painter,
                                                  const SnowCanvasRenderContext& context) {
    Q_UNUSED(painter);
    Q_UNUSED(context);
}

void SnowCanvasCustomRenderer::renderAfterCanvas(QPainter& painter,
                                                 const SnowCanvasRenderContext& context) {
    Q_UNUSED(painter);
    Q_UNUSED(context);
}
