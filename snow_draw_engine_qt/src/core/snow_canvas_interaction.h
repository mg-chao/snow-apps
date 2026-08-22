#pragma once

#include "snow_draw_engine.h"

#include <QCursor>

#include <cstdint>
#include <optional>

class QWidget;

namespace snow_canvas_interaction {

class Controller final {
  public:
    bool isEnabled() const;
    std::uint32_t capturedPointerId() const;

    void setEnabled(QWidget& widget, bool enabled);
    void setBaselineCursor(QWidget& widget, SnowCursorStyle style);
    void setCursor(QWidget& widget, SnowCursorStyle style);
    void clearTransientState(QWidget& widget);
    void applyOutput(QWidget& widget, const SnowInteractionOutput& output);

  private:
    void releasePointerCapture(QWidget& widget);
    void applyDesiredCursor(QWidget& widget, bool force = false);
    void relinquishCursor(QWidget& widget);

    bool m_enabled = true;
    std::uint32_t m_capturedPointerId = 0;
    SnowCursorStyle m_baselineCursor = SNOW_CURSOR_STYLE_DEFAULT;
    SnowCursorStyle m_desiredCursor = SNOW_CURSOR_STYLE_DEFAULT;
    std::optional<SnowCursorStyle> m_appliedCursor;
    std::optional<QCursor> m_appliedNativeCursor;
    double m_appliedDevicePixelRatio = 0.0;
};

} // namespace snow_canvas_interaction
