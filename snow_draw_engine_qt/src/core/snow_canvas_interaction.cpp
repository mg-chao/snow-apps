#include "snow_canvas_interaction.h"

#include "snow_canvas_cursor_controller.h"
#include "snow_canvas_input_adapter.h"

#include <QWidget>

namespace snow_canvas_interaction {

bool Controller::isEnabled() const {
    return m_enabled;
}


void Controller::setEnabled(QWidget& widget, SnowCanvasCursorController& cursorController,
                            bool enabled) {
    if (m_enabled == enabled) {
        return;
    }

    m_enabled = enabled;
    if (!m_enabled) {
        clearTransientState(widget, cursorController);
    }
}

void Controller::clearTransientState(QWidget& widget,
                                     SnowCanvasCursorController& cursorController) {
    if (m_capturedPointerId != 0) {
        widget.releaseMouse();
        m_capturedPointerId = 0;
    }
    cursorController.clearCursor(SnowCanvasCursorLayer::CanvasTool);
}

void Controller::applyOutput(QWidget& widget, SnowCanvasCursorController& cursorController,
                             const SnowInteractionOutput& output) {
    if (!m_enabled) {
        return;
    }

    switch (output.capture_kind) {
    case SNOW_POINTER_CAPTURE_CAPTURE:
        if (m_capturedPointerId == 0) {
            widget.grabMouse();
        }
        m_capturedPointerId = output.capture_pointer_id;
        break;
    case SNOW_POINTER_CAPTURE_RELEASE:
        if (m_capturedPointerId != 0) {
            widget.releaseMouse();
        }
        m_capturedPointerId = 0;
        break;
    case SNOW_POINTER_CAPTURE_NO_CHANGE:
    default:
        break;
    }

    if (output.cursor_kind == SNOW_CURSOR_SET) {
        cursorController.setCursor(
            SnowCanvasCursorLayer::CanvasTool,
            snow_canvas_input::cursorForSnowCursor(output.cursor_style, widget.devicePixelRatioF()));
    }
}

} // namespace snow_canvas_interaction
