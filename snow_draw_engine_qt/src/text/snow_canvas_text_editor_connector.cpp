#include "snow_canvas_text_editor_connector.h"

#include "snow_canvas_element_id.h"
#include "snow_canvas_render_geometry.h"

#include <QPointF>
#include <QRectF>

#include <cmath>

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

struct SerialTextConnection {
    QPointF start;
    QPointF end;
    QPointF baselineStart;
    QPointF baselineEnd;
    bool hasBaseline = false;
};

QRectF canvasBoundsForTextItem(const SnowSceneDisplayItem& item) {
    return snow_canvas_render_geometry::rotatedRectBounds(QPointF(item.center_x, item.center_y),
                                                          item.width, item.height, item.rotation,
                                                          item.stroke_width);
}

QRectF canvasBoundsForSerialNumberItem(const SnowSceneDisplayItem& item) {
    const double diameter = qMin(item.width, item.height);
    if (!std::isfinite(diameter) || diameter <= 0.0) {
        return {};
    }
    return snow_canvas_render_geometry::rotatedRectBounds(QPointF(item.center_x, item.center_y),
                                                          diameter, diameter, item.rotation,
                                                          item.stroke_width);
}

bool resolveSerialTextConnection(const SnowSceneDisplayItem& serial,
                                 const SnowSceneDisplayItem& text,
                                 SerialTextConnection* outConnection) {
    const double lineWidth = serial.stroke_width;
    const double diameter = qMin(serial.width, serial.height);
    if (outConnection == nullptr || lineWidth <= 0.0 || diameter <= 0.0 || text.width <= 0.0 ||
        text.height <= 0.0) {
        return false;
    }

    const QRectF serialBounds = canvasBoundsForSerialNumberItem(serial);
    const QRectF textBounds = canvasBoundsForTextItem(text);
    if (serialBounds.isEmpty() || textBounds.isEmpty()) {
        return false;
    }

    const QPointF center(serial.center_x, serial.center_y);
    const bool isAbove = textBounds.bottom() < serialBounds.top();
    const bool isBelow = textBounds.top() > serialBounds.bottom();
    const bool centeredHorizontally =
        center.x() >= textBounds.left() && center.x() <= textBounds.right();

    QPointF anchor;
    SerialTextConnection connection;
    if (centeredHorizontally && isAbove) {
        anchor = QPointF(center.x(), textBounds.bottom());
    } else if (centeredHorizontally && isBelow) {
        anchor = QPointF(center.x(), textBounds.top());
    } else {
        const double anchorX = qBound(textBounds.left(), center.x(), textBounds.right());
        const double baselineY = textBounds.bottom();
        anchor = QPointF(anchorX, baselineY);
        connection.baselineStart = QPointF(textBounds.left(), baselineY);
        connection.baselineEnd = QPointF(textBounds.right(), baselineY);
        connection.hasBaseline = true;
    }

    const double dx = anchor.x() - center.x();
    const double dy = anchor.y() - center.y();
    const double distance = std::sqrt(dx * dx + dy * dy);
    const double startOffset = diameter / 2.0 + 8.0;
    const double halfLineWidth = lineWidth / 2.0;
    if (distance <= startOffset + halfLineWidth) {
        return false;
    }

    const double ux = dx / distance;
    const double uy = dy / distance;
    connection.start = QPointF(center.x() + ux * startOffset, center.y() + uy * startOffset);
    connection.end = QPointF(anchor.x() - ux * halfLineWidth, anchor.y() - uy * halfLineWidth);
    *outConnection = connection;
    return true;
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

    SerialTextConnection connection;
    if (!resolveSerialTextConnection(serial, preview, &connection)) {
        return false;
    }

    SnowCanvasSceneItem connector;
    connector.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR;
    connector.element_id = serial.element_id;
    connector.center_x = connection.start.x();
    connector.center_y = connection.start.y();
    connector.width = connection.end.x();
    connector.height = connection.end.y();
    connector.stroke = serial.stroke;
    connector.stroke_width = serial.stroke_width;
    connector.opacity = serial.opacity;
    if (connection.hasBaseline) {
        const SnowArrowPoint points[] = {
            {connection.baselineStart.x(), connection.baselineStart.y()},
            {connection.baselineEnd.x(), connection.baselineEnd.y()},
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
