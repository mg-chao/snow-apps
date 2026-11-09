#pragma once

#include <QColor>
#include <QByteArray>
#include <optional>

#include <QCursor>
#include <QPointF>
#include <Qt>

#include <cstdint>

#include "snow_draw_engine.h"

class QKeyEvent;
class QMouseEvent;
class QTabletEvent;
class QWheelEvent;
class QFont;
class SnowCanvasSceneItem;

namespace snow_canvas_input {

QCursor strokeCursor(double diameter, const std::optional<QColor>& color, bool crosshair,
                     qreal devicePixelRatio = 1.0);
QCursor cursorForSnowCursor(SnowCursorStyle style, qreal devicePixelRatio = 1.0);
QCursor serialNumberCursor(const SnowCanvasSceneItem& preview, const QFont& font, double zoom,
                           qreal devicePixelRatio = 1.0);
QByteArray serialNumberCursorKey(const SnowCanvasSceneItem& preview, const QFont& font,
                                 double zoom);

SnowInputEvent makePointerInput(const QMouseEvent& event, SnowPointerEventType eventType);
SnowInputEvent makePointerInput(const QTabletEvent& event, SnowPointerEventType eventType);
SnowInputEvent makePointerInput(const QPointF& position, Qt::MouseButton button,
                                Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers,
                                SnowPointerEventType eventType);
SnowInputEvent makeWheelInput(const QWheelEvent& event);
SnowInputEvent makeKeyInput(const QKeyEvent& event, SnowKeyEventType eventType);

SnowPointerButton mapButton(Qt::MouseButton button);
std::uint8_t mapButtons(Qt::MouseButtons buttons);
SnowModifiers mapModifiers(Qt::KeyboardModifiers modifiers);
SnowKeyCode mapKeyCode(const QKeyEvent& event, std::uint32_t* outCodepoint);

} // namespace snow_canvas_input
