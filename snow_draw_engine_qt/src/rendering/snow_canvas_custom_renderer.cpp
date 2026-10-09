#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

std::uint64_t SnowCanvasCustomRenderer::contentRevision() const {
    return 0;
}

std::uint64_t SnowCanvasCustomRenderer::originalBackgroundRevision() const {
    return contentRevision();
}

void SnowCanvasCustomRenderer::clearRenderState() {}

std::optional<SnowCanvasFilterRenderReference>
SnowCanvasCustomRenderer::filterRenderReference() const {
    return std::nullopt;
}

bool SnowCanvasCustomRenderer::coversWidgetRect(const QRect& widgetRect) const {
    Q_UNUSED(widgetRect);
    return false;
}

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

void SnowCanvasCustomRenderer::renderOriginalBackground(QPainter& painter,
                                                        const SnowCanvasRenderContext& context) {
    renderBeforeCanvas(painter, context);
}
