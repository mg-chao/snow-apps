#include "snow_canvas_input_adapter.h"
#include "snow_draw_engine_qt/snow_canvas_cursor.h"
#include "icons/draw_engine_icons.h"
#include "icon_renderer.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"

#include <QKeyEvent>
#include <QPainter>
#include <QPixmap>
#include <QDataStream>
#include <QIODevice>
#include <cmath>
#include <algorithm>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QWheelEvent>

QCursor snowCanvasCornerRadiusCursor(qreal devicePixelRatio) {
    constexpr int kCursorLogicalSize = 32;
    constexpr QPoint kHotSpot(3, 3);
    return adqt::icons::makeCursor(snow::draw_engine::icons::cursor::CornerRadius(),
                                   QSize(kCursorLogicalSize, kCursorLogicalSize), kHotSpot,
                                   devicePixelRatio);
}

namespace snow_canvas_input {

QByteArray serialNumberCursorKey(const SnowCanvasSceneItem& item, const QFont& font, double zoom) {
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    const auto color = [&stream](SnowColorRgba8 value) {
        stream << value.r << value.g << value.b << value.a;
    };
    const bool rough = (item.serial_number_type == SNOW_SERIAL_NUMBER_TYPE_OUTLINED_CIRCLE ||
                        item.serial_number_type == SNOW_SERIAL_NUMBER_TYPE_OUTLINED_SQUARE) &&
                       item.stroke_rendering_style != SNOW_STROKE_RENDERING_STYLE_ARCHITECT;
    stream << font << zoom << item.width << item.height << item.rotation << item.opacity
           << item.font_size << item.stroke_width << quint32(item.serial_number_type)
           << quint32(item.fill_style) << quint32(item.stroke_style)
           << quint32(item.stroke_rendering_style) << (rough ? item.stroke_rendering_seed : 0u)
           << item.corner_radii.top_left
           << QByteArray(item.text_utf8, static_cast<qsizetype>(item.text_utf8_len))
           << QByteArray(item.font_family_utf8, static_cast<qsizetype>(item.font_family_utf8_len));
    color(item.stroke);
    color(item.fill);
    color(item.text_color);
    return key;
}

QCursor serialNumberCursor(const SnowCanvasSceneItem& preview, const QFont& font, double zoom,
                           qreal devicePixelRatio) {
    SnowCanvasSceneItem item(preview);
    item.center_x = 0.0;
    item.center_y = 0.0;
    SceneDisplayInfo info;
    info.camera_zoom = std::isfinite(zoom) && zoom > 0.0 ? zoom : 1.0;
    QRectF bounds = snow_canvas_render_geometry::sceneItemBounds(info, item);
    double extent = 2.0 * std::max({std::abs(bounds.left()), std::abs(bounds.right()),
                                    std::abs(bounds.top()), std::abs(bounds.bottom())});
    if (!std::isfinite(extent) || extent <= 0.0) {
        return QCursor(Qt::CrossCursor);
    }
    // Match the stroke cursor's allocation guard, scaling the entire badge together.
    if (extent > 1024.0) {
        info.camera_zoom *= 1024.0 / extent;
        extent = 1024.0;
    }
    const qreal dpr =
        std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const int radius = static_cast<int>(std::ceil(extent / 2.0)) + 2;
    const int size = radius * 2 + 1;
    QPixmap pixmap(static_cast<int>(std::ceil(size * dpr)),
                   static_cast<int>(std::ceil(size * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    info.surface_width = radius * 2;
    info.surface_height = radius * 2;
    QPainter painter(&pixmap);
    painter.setFont(font);
    painter.setRenderHint(QPainter::Antialiasing);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = &item;
    request.sceneItemCount = 1;
    request.exposedRegion = QRegion(0, 0, size, size);
    snow_canvas_renderer::renderSceneItems(request);
    painter.end();
    return QCursor(pixmap, radius, radius);
}

QCursor strokeCursor(double diameter, const std::optional<QColor>& color, bool crosshair,
                     qreal devicePixelRatio) {
    diameter = std::isfinite(diameter) ? std::clamp(diameter, 1.0, 1024.0) : 1.0;
    const qreal dpr =
        std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const int radius = static_cast<int>(std::ceil(diameter / 2.0 + (crosshair ? 12.0 : 2.0)));
    const int size = radius * 2 + 1;
    QPixmap pixmap(static_cast<int>(std::ceil(size * dpr)),
                   static_cast<int>(std::ceil(size * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(radius, radius);
    const QColor outline(0, 0, 0, 140);
    painter.setPen(QPen(outline, 1.5));
    painter.setBrush(color.value_or(QColor(255, 255, 255, 20)));
    painter.drawEllipse(QRectF(-diameter / 2.0, -diameter / 2.0, diameter, diameter));
    if (crosshair) {
        const QColor arm = color.value_or(outline);
        const int haloChannel =
            0.299 * arm.red() + 0.587 * arm.green() + 0.114 * arm.blue() > 128.0 ? 0 : 255;
        const QColor halo(haloChannel, haloChannel, haloChannel, 140);
        const qreal inner = diameter / 2.0 + 4.0;
        const qreal outer = inner + 6.0;
        for (const QPointF& direction :
             {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) {
            painter.setPen(QPen(halo, 2.0));
            painter.drawLine(direction * inner, direction * outer);
            painter.setPen(QPen(arm, 1.0));
            painter.drawLine(direction * inner, direction * outer);
        }
    }
    painter.end();
    return QCursor(pixmap, radius, radius);
}

QCursor cursorForSnowCursor(SnowCursorStyle style, qreal devicePixelRatio) {
    switch (style) {
    case SNOW_CURSOR_STYLE_STROKE:
        return strokeCursor(2.0, std::nullopt, true, devicePixelRatio);
    case SNOW_CURSOR_STYLE_ERASER:
        return strokeCursor(16.0, std::nullopt, false, devicePixelRatio);
    case SNOW_CURSOR_STYLE_CROSSHAIR:
    case SNOW_CURSOR_STYLE_SERIAL_NUMBER:
        return QCursor(Qt::CrossCursor);
    case SNOW_CURSOR_STYLE_CORNER_RADIUS:
        return snowCanvasCornerRadiusCursor(devicePixelRatio);
    case SNOW_CURSOR_STYLE_GRAB:
    case SNOW_CURSOR_STYLE_GRABBING:
#ifdef Q_OS_WIN
        return QCursor(Qt::PointingHandCursor);
#else
        return QCursor(style == SNOW_CURSOR_STYLE_GRAB ? Qt::OpenHandCursor : Qt::ClosedHandCursor);
#endif
    case SNOW_CURSOR_STYLE_MOVE:
        return QCursor(Qt::SizeAllCursor);
    case SNOW_CURSOR_STYLE_RESIZE_HORIZONTAL:
        return QCursor(Qt::SizeHorCursor);
    case SNOW_CURSOR_STYLE_RESIZE_VERTICAL:
        return QCursor(Qt::SizeVerCursor);
    case SNOW_CURSOR_STYLE_RESIZE_NWSE:
        return QCursor(Qt::SizeFDiagCursor);
    case SNOW_CURSOR_STYLE_RESIZE_NESW:
        return QCursor(Qt::SizeBDiagCursor);
    case SNOW_CURSOR_STYLE_TEXT:
        return QCursor(Qt::IBeamCursor);
    case SNOW_CURSOR_STYLE_NOT_ALLOWED:
        return QCursor(Qt::ForbiddenCursor);
    case SNOW_CURSOR_STYLE_HIDDEN:
        return QCursor(Qt::BlankCursor);
    case SNOW_CURSOR_STYLE_DEFAULT:
    default:
        return QCursor(Qt::ArrowCursor);
    }
}

SnowInputEvent makePointerInput(const QMouseEvent& event, SnowPointerEventType eventType) {
    auto input = makePointerInput(event.position(), event.button(), event.buttons(),
                                  event.modifiers(), eventType);
    input.pointer.timestamp_ms = event.timestamp();
    return input;
}

SnowInputEvent makePointerInput(const QTabletEvent& event, SnowPointerEventType eventType) {
    auto input = makePointerInput(event.position(), event.button(), event.buttons(),
                                  event.modifiers(), eventType);
    input.pointer.pointer_id = 2;
    input.pointer.device = SNOW_POINTER_DEVICE_PEN;
    input.pointer.timestamp_ms = event.timestamp();
    if (event.pointingDevice() != nullptr &&
        event.pointingDevice()->capabilities().testFlag(QInputDevice::Capability::Pressure) &&
        std::isfinite(event.pressure())) {
        input.pointer.has_pressure = 1;
        input.pointer.pressure = std::clamp(event.pressure(), 0.0, 1.0);
    }
    return input;
}

SnowInputEvent makePointerInput(const QPointF& position, Qt::MouseButton button,
                                Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers,
                                SnowPointerEventType eventType) {
    SnowInputEvent input{};
    input.kind = SNOW_INPUT_EVENT_POINTER;
    input.pointer.pointer_id = 1;
    input.pointer.event_type = eventType;
    input.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
    input.pointer.position_x = position.x();
    input.pointer.position_y = position.y();
    input.pointer.button = mapButton(button);
    input.pointer.buttons = mapButtons(buttons);
    input.pointer.modifiers = mapModifiers(modifiers);
    return input;
}

SnowInputEvent makeWheelInput(const QWheelEvent& event) {
    SnowInputEvent input{};
    input.kind = SNOW_INPUT_EVENT_WHEEL;
    input.wheel.position_x = event.position().x();
    input.wheel.position_y = event.position().y();
    const QPoint delta = !event.pixelDelta().isNull() ? event.pixelDelta() : event.angleDelta();
    input.wheel.delta_x = delta.x();
    input.wheel.delta_y = delta.y();
    input.wheel.delta_kind =
        event.pixelDelta().isNull() ? SNOW_WHEEL_DELTA_ANGLE : SNOW_WHEEL_DELTA_PIXEL;
    input.wheel.modifiers = mapModifiers(event.modifiers());
    return input;
}

SnowInputEvent makeKeyInput(const QKeyEvent& event, SnowKeyEventType eventType) {
    SnowInputEvent input{};
    input.kind = SNOW_INPUT_EVENT_KEY;
    input.key.event_type = eventType;
    input.key.key_code = mapKeyCode(event, &input.key.codepoint);
    input.key.modifiers = mapModifiers(event.modifiers());
    input.key.repeat = event.isAutoRepeat();
    return input;
}

SnowPointerButton mapButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton:
        return SNOW_POINTER_BUTTON_PRIMARY;
    case Qt::RightButton:
        return SNOW_POINTER_BUTTON_SECONDARY;
    case Qt::MiddleButton:
        return SNOW_POINTER_BUTTON_MIDDLE;
    case Qt::NoButton:
    default:
        return SNOW_POINTER_BUTTON_NONE;
    }
}

std::uint8_t mapButtons(Qt::MouseButtons buttons) {
    std::uint8_t value = 0;
    if (buttons.testFlag(Qt::LeftButton)) {
        value |= 0b0000'0001;
    }
    if (buttons.testFlag(Qt::RightButton)) {
        value |= 0b0000'0010;
    }
    if (buttons.testFlag(Qt::MiddleButton)) {
        value |= 0b0000'0100;
    }
    return value;
}

SnowModifiers mapModifiers(Qt::KeyboardModifiers modifiers) {
    SnowModifiers value{};
    value.ctrl = modifiers.testFlag(Qt::ControlModifier);
    value.shift = modifiers.testFlag(Qt::ShiftModifier);
    value.alt = modifiers.testFlag(Qt::AltModifier);
    value.meta = modifiers.testFlag(Qt::MetaModifier);
    return value;
}

SnowKeyCode mapKeyCode(const QKeyEvent& event, std::uint32_t* outCodepoint) {
    if (outCodepoint != nullptr) {
        *outCodepoint = 0;
    }

    switch (event.key()) {
    case Qt::Key_Space:
        return SNOW_KEY_CODE_SPACE;
    case Qt::Key_Escape:
        return SNOW_KEY_CODE_ESCAPE;
    case Qt::Key_Up:
        return SNOW_KEY_CODE_ARROW_UP;
    case Qt::Key_Down:
        return SNOW_KEY_CODE_ARROW_DOWN;
    case Qt::Key_Left:
        return SNOW_KEY_CODE_ARROW_LEFT;
    case Qt::Key_Right:
        return SNOW_KEY_CODE_ARROW_RIGHT;
    case Qt::Key_Backspace:
        return SNOW_KEY_CODE_BACKSPACE;
    case Qt::Key_Delete:
        return SNOW_KEY_CODE_DELETE;
    default:
        break;
    }

    const QString text = event.text();
    const auto codepoints = text.toUcs4();
    if (outCodepoint != nullptr && codepoints.size() == 1 && !text.isEmpty() &&
        text.front().isPrint()) {
        *outCodepoint = codepoints.front();
        return SNOW_KEY_CODE_CHARACTER;
    }

    return SNOW_KEY_CODE_UNKNOWN;
}

} // namespace snow_canvas_input
