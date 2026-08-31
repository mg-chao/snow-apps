#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_draw_engine.h"

#include <QRegion>

#include <cstdint>

namespace snow_canvas_text_editor_connector {

bool itemBindsPreview(const SnowSceneDisplayItem& item, const SnowSceneDisplayItem* preview);
bool connectorBelongsToPreview(const SnowSceneDisplayItem& connector,
                               const SnowCanvasSceneItem* sceneItems, std::uint32_t sceneItemCount,
                               const SnowSceneDisplayItem* preview);
bool connectorItemForPreview(const SnowSceneDisplayItem& serial,
                             const SnowSceneDisplayItem& preview,
                             SnowCanvasSceneItem* outConnector);
QRegion connectorRegion(const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem* sceneItems,
                        std::uint32_t sceneItemCount, const SnowSceneDisplayItem* preview);

} // namespace snow_canvas_text_editor_connector
