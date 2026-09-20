#include "snow_canvas_cursor_controller.h"

#include "snow_draw_engine_qt/snow_canvas_view.h"
#include "snow_canvas_input_adapter.h"

#include <algorithm>
#include <cmath>

SnowCanvasCursorController::SnowCanvasCursorController(SnowCanvasView& widget) : m_widget(widget) {}

void SnowCanvasCursorController::setCursor(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    if (layer == SnowCanvasCursorLayer::CanvasTool) {
        m_engineCursorStyle.reset();
    }
    cursorForLayer(layer) = cursor;
    applyResolvedCursor();
}

void SnowCanvasCursorController::clearCursor(SnowCanvasCursorLayer layer) {
    if (layer == SnowCanvasCursorLayer::CanvasTool) {
        m_engineCursorStyle.reset();
    }
    cursorForLayer(layer).reset();
    applyResolvedCursor();
}

std::optional<QCursor>& SnowCanvasCursorController::cursorForLayer(SnowCanvasCursorLayer layer) {
    switch (layer) {
    case SnowCanvasCursorLayer::Host:
        return m_hostCursor;
    case SnowCanvasCursorLayer::CanvasTool:
    default:
        return m_canvasToolCursor;
    }
}

void SnowCanvasCursorController::applyResolvedCursor() {
    if (m_hostCursor.has_value()) {
        applyResolvedCursorToWidget(*m_hostCursor);
        return;
    }
    if (m_canvasToolCursor.has_value()) {
        applyResolvedCursorToWidget(*m_canvasToolCursor);
        return;
    }
    if (m_widget.hasCursor()) {
        m_widget.unsetCursor();
    }
}

void SnowCanvasCursorController::applyResolvedCursorToWidget(const QCursor& cursor) {
    // QWidget forwards every cursor change to the platform window, and on
    // Windows each changed-shape transition reaches the native sprite
    // immediately. Re-applying the cursor a widget already shows would flash
    // it, so layered updates must resolve to a no-op here.
    if (m_widget.hasCursor() && m_widget.cursor() == cursor) {
        return;
    }
    m_widget.setCursor(cursor);
}

void SnowCanvasCursorController::setEngineCursor(SnowCursorStyle style) {
    const qreal dpr = m_widget.devicePixelRatioF();
    if (dpr != m_cursorDevicePixelRatio) {
        m_cursorDevicePixelRatio = dpr;
        m_strokeCursor.reset();
        m_eraserCursor.reset();
    }
    if (style == SNOW_CURSOR_STYLE_STROKE || style == SNOW_CURSOR_STYLE_ERASER) {
        auto& cached = style == SNOW_CURSOR_STYLE_STROKE ? m_strokeCursor : m_eraserCursor;
        if (!cached) {
            cached = snow_canvas_input::strokeCursor(
                style == SNOW_CURSOR_STYLE_STROKE ? m_strokeDiameter : 16.0,
                style == SNOW_CURSOR_STYLE_STROKE ? m_strokeColor : std::nullopt,
                style == SNOW_CURSOR_STYLE_STROKE, dpr);
        }
        m_canvasToolCursor = *cached;
    } else {
        m_canvasToolCursor = snow_canvas_input::cursorForSnowCursor(style, dpr);
    }
    m_engineCursorStyle = style;
    applyResolvedCursor();
}

void SnowCanvasCursorController::configureStrokeCursor(double diameter,
                                                       const std::optional<QColor>& color) {
    // Bound native bitmap allocation even at extreme canvas zoom levels.
    diameter = std::isfinite(diameter) ? std::clamp(diameter, 1.0, 1024.0) : 1.0;
    if (m_strokeDiameter == diameter && m_strokeColor == color &&
        m_cursorDevicePixelRatio == m_widget.devicePixelRatioF()) {
        return;
    }
    m_strokeDiameter = diameter;
    m_strokeColor = color;
    m_strokeCursor.reset();
    refreshDevicePixelRatio();
}

void SnowCanvasCursorController::refreshDevicePixelRatio() {
    if (m_engineCursorStyle) {
        setEngineCursor(*m_engineCursorStyle);
    }
}
