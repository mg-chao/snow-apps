#pragma once

#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_image_source_types.h"

#include <QList>
#include <QPainterPath>

#include <cstddef>

class QPainter;
struct SceneDisplayInfo;

namespace snow_canvas_magnifier_renderer {

struct RenderDiagnostics {
    std::size_t renderedMagnifierCount = 0;
    std::size_t sourceLayerDrawCount = 0;
};

RenderDiagnostics diagnosticsForCurrentThread();
void resetDiagnosticsForCurrentThread();
QPainterPath lensPath(const SnowSceneDisplayItem& item);
void render(QPainter& painter, const SceneDisplayInfo& displayInfo,
            const SnowSceneDisplayItem& item, const QList<SnowCanvasBaseImageSource>* sources);

} // namespace snow_canvas_magnifier_renderer
