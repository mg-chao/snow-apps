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
    if (m_widget.testAttribute(Qt::WA_SetCursor)) {
        m_widget.unsetCursor();
    }
}

void SnowCanvasCursorController::applyResolvedCursorToWidget(const QCursor& cursor) {
    // QWidget forwards every cursor change to the platform window, and on
    // Windows each changed-shape transition reaches the native sprite
    // immediately. Re-applying the cursor a widget already shows would flash
    // it, so layered updates must resolve to a no-op here.
    if (m_widget.testAttribute(Qt::WA_SetCursor) && m_widget.cursor() == cursor) {
        return;
    }
    m_widget.setCursor(cursor);
}
