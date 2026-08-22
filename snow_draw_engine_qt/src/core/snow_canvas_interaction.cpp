#include "snow_canvas_interaction.h"

#include "snow_canvas_input_adapter.h"

#include <QWidget>
#include <QtGlobal>

namespace snow_canvas_interaction {

bool Controller::isEnabled() const {
    return m_enabled;
}

std::uint32_t Controller::capturedPointerId() const {
    return m_capturedPointerId;
}

void Controller::setEnabled(QWidget& widget, bool enabled) {
    if (m_enabled == enabled) {
        return;
    }

    m_enabled = enabled;
    if (!m_enabled) {
        clearTransientState(widget);
        return;
    }

    m_desiredCursor = m_baselineCursor;
    applyDesiredCursor(widget, true);
}

void Controller::setBaselineCursor(QWidget& widget, SnowCursorStyle style) {
    m_baselineCursor = style;
    m_desiredCursor = style;
    applyDesiredCursor(widget);
}

void Controller::setCursor(QWidget& widget, SnowCursorStyle style) {
    m_desiredCursor = style;
    applyDesiredCursor(widget);
}

void Controller::clearTransientState(QWidget& widget) {
    releasePointerCapture(widget);
    m_desiredCursor = m_baselineCursor;
    relinquishCursor(widget);
}

void Controller::applyOutput(QWidget& widget, const SnowInteractionOutput& output) {
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
        setCursor(widget, output.cursor_style);
    }
}

void Controller::releasePointerCapture(QWidget& widget) {
    if (m_capturedPointerId == 0) {
        return;
    }
    widget.releaseMouse();
    m_capturedPointerId = 0;
}

void Controller::applyDesiredCursor(QWidget& widget, bool force) {
    if (!m_enabled) {
        return;
    }

    const double devicePixelRatio = widget.devicePixelRatioF();
    if (!force && m_appliedCursor == m_desiredCursor &&
        qFuzzyCompare(m_appliedDevicePixelRatio, devicePixelRatio) &&
        m_appliedNativeCursor.has_value() && widget.testAttribute(Qt::WA_SetCursor) &&
        widget.cursor() == *m_appliedNativeCursor) {
        return;
    }

    const QCursor nativeCursor =
        snow_canvas_input::cursorForSnowCursor(m_desiredCursor, devicePixelRatio);
    if (!force && widget.testAttribute(Qt::WA_SetCursor) && widget.cursor() == nativeCursor) {
        m_appliedCursor = m_desiredCursor;
        m_appliedNativeCursor = nativeCursor;
        m_appliedDevicePixelRatio = devicePixelRatio;
        return;
    }

    widget.setCursor(nativeCursor);
    m_appliedCursor = m_desiredCursor;
    m_appliedNativeCursor = nativeCursor;
    m_appliedDevicePixelRatio = devicePixelRatio;
}

void Controller::relinquishCursor(QWidget& widget) {
    if (widget.testAttribute(Qt::WA_SetCursor)) {
        widget.unsetCursor();
    }
    m_appliedCursor.reset();
    m_appliedNativeCursor.reset();
    m_appliedDevicePixelRatio = 0.0;
}

} // namespace snow_canvas_interaction
