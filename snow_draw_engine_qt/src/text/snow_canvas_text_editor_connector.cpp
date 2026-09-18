#include "snow_canvas_text_editor_connector.h"

#include "snow_canvas_element_id.h"
#include "snow_canvas_render_geometry.h"

#include <cstdint>

namespace snow_canvas_text_editor_connector {
namespace {

SnowElementId boundTextElementId(const SnowSceneDisplayItem& item) {
    return item.has_bound_text_element != 0
        ? SnowElementId {
            item.bound_text_element_index,
            item.bound_text_element_generation,
        }
        : SnowElementId {};
}

const SnowSceneDisplayItem* findSerialNumberItem(const SnowCanvasSceneItem* sceneItems,
                                                 std::uint32_t sceneItemCount,
                                                 const SnowElementId& id) {
    if (sceneItems == nullptr || !snow_canvas_element_id::hasElementId(id)) {
        return nullptr;
    }
    for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
        const SnowSceneDisplayItem& item = sceneItems[index];
        if (item.kind == SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER &&
            snow_canvas_element_id::sameElementId(item.element_id, id)) {
            return &item;
        }
    }
    return nullptr;
}

} // namespace

bool itemBindsPreview(const SnowSceneDisplayItem& item, const SnowSceneDisplayItem* preview) {
    if (preview == nullptr || preview->kind != SNOW_SCENE_DISPLAY_ITEM_TEXT ||
        !snow_canvas_element_id::hasElementId(preview->element_id)) {
        return false;
    }
    return snow_canvas_element_id::sameElementId(boundTextElementId(item), preview->element_id);
}

bool connectorBelongsToPreview(const SnowSceneDisplayItem& connector,
                               const SnowCanvasSceneItem* sceneItems, std::uint32_t sceneItemCount,
                               const SnowSceneDisplayItem* preview) {
    if (connector.kind != SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR) {
        return false;
    }
    const SnowSceneDisplayItem* serial =
        findSerialNumberItem(sceneItems, sceneItemCount, connector.element_id);
    return serial != nullptr && itemBindsPreview(*serial, preview);
}

bool connectorItemForPreview(const SnowSceneDisplayItem& serial,
                             const SnowSceneDisplayItem& preview,
                             SnowCanvasSceneItem* outConnector) {
    if (outConnector == nullptr || serial.kind != SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER ||
        !itemBindsPreview(serial, &preview)) {
        return false;
    }

    SnowSerialTextConnection connection{};
    if (snow_scene_resolve_serial_text_connection(&serial, &preview, &connection) == 0) {
        return false;
    }

    SnowCanvasSceneItem connector;
    connector.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR;
    connector.element_id = serial.element_id;
    connector.center_x = connection.start_x;
    connector.center_y = connection.start_y;
    connector.width = connection.end_x;
    connector.height = connection.end_y;
    connector.stroke = serial.stroke;
    connector.stroke_width = serial.stroke_width;
    connector.opacity = serial.opacity;
    if (connection.has_baseline != 0) {
        const SnowArrowPoint points[] = {
            {connection.baseline_start_x, connection.baseline_start_y},
            {connection.baseline_end_x, connection.baseline_end_y},
        };
        connector.setArrowPoints(points, 2);
    }
    *outConnector = connector;
    return true;
}

QRegion connectorRegion(const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem* sceneItems,
                        std::uint32_t sceneItemCount, const SnowSceneDisplayItem* preview) {
    QRegion region;
    if (sceneItems == nullptr || preview == nullptr ||
        preview->kind != SNOW_SCENE_DISPLAY_ITEM_TEXT ||
        !snow_canvas_element_id::hasElementId(preview->element_id)) {
        return region;
    }

    for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
        const SnowSceneDisplayItem& item = sceneItems[index];
        if (connectorBelongsToPreview(item, sceneItems, sceneItemCount, preview)) {
            region += snow_canvas_render_geometry::alignedRectForBounds(
                snow_canvas_render_geometry::sceneItemBounds(displayInfo, item));
        }
        if (item.kind != SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER ||
            !itemBindsPreview(item, preview)) {
            continue;
        }

        SnowCanvasSceneItem connector;
        if (!connectorItemForPreview(item, *preview, &connector)) {
            continue;
        }
        region += snow_canvas_render_geometry::alignedRectForBounds(
            snow_canvas_render_geometry::sceneItemBounds(displayInfo, connector));
    }

    return region;
}

} // namespace snow_canvas_text_editor_connector
