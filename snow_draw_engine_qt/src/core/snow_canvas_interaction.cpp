#include "snow_canvas_interaction.h"

#include "snow_canvas_cursor_controller.h"
#include "snow_canvas_input_adapter.h"

#include "snow_draw_engine_qt/snow_canvas_view.h"

namespace snow_canvas_interaction {

bool Controller::isEnabled() const {
    return m_enabled;
}

void Controller::setEnabled(SnowCanvasView& widget, SnowCanvasCursorController& cursorController,
                            bool enabled) {
    if (m_enabled == enabled) {
        return;
    }

    m_enabled = enabled;
    if (!m_enabled) {
        clearTransientState(widget, cursorController);
    }
}

void Controller::clearTransientState(SnowCanvasView& widget,
                                     SnowCanvasCursorController& cursorController) {
    if (m_capturedPointerId != 0) {
        widget.releaseMouse();
        m_capturedPointerId = 0;
    }
    cursorController.clearCursor(SnowCanvasCursorLayer::CanvasTool);
}

void Controller::applyOutput(SnowCanvasView& widget, SnowCanvasCursorController& cursorController,
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
        cursorController.setEngineCursor(output.cursor_style);
    }
}

} // namespace snow_canvas_interaction
