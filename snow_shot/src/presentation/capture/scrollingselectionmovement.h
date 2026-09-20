#pragma once

#include "snow_shot/presentation/screenshotscrollingtypes.h"
#include <QPoint>
#include <QRect>
#include <algorithm>

namespace snow_shot::capture_detail {
// All coordinates are physical canvas pixels; the desktop may span multiple monitors.
class ScrollingSelectionMovement {
  public:
    bool begin(ScreenshotScrollingRecognitionMode requestedAxis,
               ScreenshotScrollingRecognitionMode captureAxis, QRect selection, QPoint pointer) {
        if (m_active || requestedAxis != captureAxis || selection.isEmpty())
            return false;
        m_active = true;
        m_axis = requestedAxis;
        m_selection = selection;
        m_pointer = pointer;
        return true;
    }
    QRect update(QPoint pointer, QRect bounds) const {
        QRect result = m_selection;
        if (!m_active || bounds.isEmpty())
            return result;
        const QPoint delta = pointer - m_pointer;
        if (m_axis == ScreenshotScrollingRecognitionMode::Horizontal)
            result.moveLeft(
                std::clamp(result.x() + delta.x(), bounds.x(),
                           std::max(bounds.x(), bounds.x() + bounds.width() - result.width())));
        else
            result.moveTop(
                std::clamp(result.y() + delta.y(), bounds.y(),
                           std::max(bounds.y(), bounds.y() + bounds.height() - result.height())));
        return result;
    }
    bool active() const {
        return m_active;
    }
    void end() {
        m_active = false;
    }

  private:
    bool m_active = false;
    ScreenshotScrollingRecognitionMode m_axis = ScreenshotScrollingRecognitionMode::Vertical;
    QRect m_selection;
    QPoint m_pointer;
};
} // namespace snow_shot::capture_detail
