#pragma once

#include "snow_shot/presentation/globalmousegesture.h"

#include <CoreGraphics/CoreGraphics.h>
#include <QRectF>

namespace snow_shot::platform::macos::detail {
struct MouseDisplayMapping {
    QRectF logicalBounds;
    qreal backingScale = 1;
};
struct NativeMouseInput {
    CGEventType type = kCGEventNull;
    QPointF position;
    CGEventFlags flags = 0;
    Qt::MouseButtons heldButtons;
    int buttonNumber = 0;
    quint16 keyCode = 0;
    bool injected = false;
};
struct NativeMouseResult {
    presentation::GlobalMouseInputResult input;
    bool normalizeDraggedMove = false;
};

// The native adapter and deterministic tests share the event translation and ownership rules.
class GlobalMouseEventProcessor final {
  public:
    void configure(const presentation::GlobalMouseConfiguration& configuration);
    void setDisplays(QVector<MouseDisplayMapping> displays);
    [[nodiscard]] QPoint physicalPosition(const QPointF& desktopPoint) const;
    [[nodiscard]] NativeMouseResult handle(const NativeMouseInput& input);
    [[nodiscard]] presentation::GlobalMouseInputResult
    beginButtonDrag(presentation::settings::SettingsGlobalMouseAction action,
                    const QPointF& desktopPoint);
    void cancel(quint64 id);
    [[nodiscard]] presentation::GlobalMouseInputResult cancelPending();
    [[nodiscard]] presentation::GlobalMouseInputResult
    cancelInterruptedInput(Qt::MouseButtons heldButtons, bool escapeHeld);
    [[nodiscard]] bool pending() const {
        return m_gesture.pending();
    }
    [[nodiscard]] bool active() const {
        return m_gesture.active();
    }
    void reset();

  private:
    presentation::GlobalMouseConfiguration m_configuration;
    presentation::GlobalMouseGesture m_gesture;
    QVector<MouseDisplayMapping> m_displays;
    QPoint m_lastPosition;
    bool m_escapeConsumed = false;
};
} // namespace snow_shot::platform::macos::detail
