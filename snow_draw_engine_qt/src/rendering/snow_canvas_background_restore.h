#pragma once

#include "snow_canvas_filter_tile_cache.h"

#include <QImage>
#include <QRect>
#include <memory>

class SnowCanvasSceneItem;
struct SceneDisplayInfo;
namespace snow_canvas_pen_mask {
class PenMaskAtlas;
}
namespace snow_canvas_filter_render {
class RenderWorkspace;
struct ExecutionOptions;
} // namespace snow_canvas_filter_render
namespace snow_canvas_renderer {
struct SceneRenderRequest;
struct FilterFrameInfo;
struct FilterRenderDiagnostics;
} // namespace snow_canvas_renderer

namespace snow_canvas_background_restore {

bool hasCoverage(const SnowCanvasSceneItem& item,
                 const snow_canvas_renderer::FilterFrameInfo& geometry,
                 const SceneDisplayInfo& info, const QRect& physicalBounds, qreal dpr,
                 snow_canvas_pen_mask::PenMaskAtlas& atlas,
                 const snow_canvas_filter_render::ExecutionOptions& execution);

const QImage*
originalBackground(const snow_canvas_renderer::SceneRenderRequest& request,
                   const QRect& physicalBounds, qreal dpr,
                   snow_canvas_filter_render::RenderWorkspace& workspace,
                   std::shared_ptr<const snow_canvas_filter_tile_cache::Entry>& retained,
                   snow_canvas_renderer::FilterRenderDiagnostics& diagnostics);

bool apply(const QImage& original, QImage& destination, const SnowCanvasSceneItem& item,
           const snow_canvas_renderer::FilterFrameInfo& geometry, const SceneDisplayInfo& info,
           const QRect& physicalBounds, qreal dpr, snow_canvas_pen_mask::PenMaskAtlas& atlas,
           snow_canvas_filter_render::RenderWorkspace& workspace,
           const snow_canvas_filter_render::ExecutionOptions& execution,
           snow_canvas_renderer::FilterRenderDiagnostics& diagnostics);

} // namespace snow_canvas_background_restore
