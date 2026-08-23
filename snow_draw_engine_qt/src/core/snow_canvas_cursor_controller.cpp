#include "snow_canvas_cursor_controller.h"

#include <QWidget>

SnowCanvasCursorController::SnowCanvasCursorController(QWidget& widget) : m_widget(widget) {}

void SnowCanvasCursorController::setCursor(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    cursorForLayer(layer) = cursor;
    applyResolvedCursor();
}

void SnowCanvasCursorController::clearCursor(SnowCanvasCursorLayer layer) {
    cursorForLayer(layer).reset();
    applyResolvedCursor();
}

std::optional<QCursor>&
SnowCanvasCursorController::cursorForLayer(SnowCanvasCursorLayer layer) {
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
        m_widget.setCursor(*m_hostCursor);
        return;
    }
    if (m_canvasToolCursor.has_value()) {
        m_widget.setCursor(*m_canvasToolCursor);
        return;
    }
    m_widget.unsetCursor();
}
