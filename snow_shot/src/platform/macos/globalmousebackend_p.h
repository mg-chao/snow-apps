#ifndef SNOW_SHOT_PLATFORM_MACOS_GLOBALMOUSEBACKEND_P_H
#define SNOW_SHOT_PLATFORM_MACOS_GLOBALMOUSEBACKEND_P_H

#include "snow_shot/presentation/globalmousemanager.h"
#include <CoreGraphics/CoreGraphics.h>

namespace snow_shot::presentation::detail {

// A coherent snapshot from one CoreGraphics input-state domain.
struct MacGlobalMouseInputState {
    Qt::MouseButtons buttons;
    CGEventFlags modifierFlags = 0;
    bool escapeDown = false;
};

// Native calls are injectable so lifecycle tests never install a system hook
// or request TCC access. Event translation is tested with private CGEvents.
struct MacGlobalMouseApi {
    std::function<bool()> listenAccess;
    std::function<bool()> accessibilityAccess;
    std::function<CFMachPortRef(CGEventMask, CGEventTapCallBack, void*)> createTap;
    std::function<void(CFMachPortRef, bool)> enableTap;
    std::function<bool(CFMachPortRef)> tapEnabled;
    std::function<MacGlobalMouseInputState()> inputState;
    std::function<std::optional<QPointF>()> cursor;
    std::function<void()> requestAccess;
    std::function<void(bool)> openSettings;
    std::function<bool()> sessionActive;
};

struct MacGlobalMouseInput {
    GlobalMouseGesture gesture;
    Qt::MouseButtons heldButtons;
    CGEventFlags maskedFlags = 0;
    bool escapeConsumed = false;
    QPointF lastPosition;

    // Mutates an event only after the session input establishes gesture ownership.
    // Source process and state identifiers describe provenance, not user intent.
    [[nodiscard]] GlobalMouseInputResult handle(CGEventType type, CGEventRef event,
                                                const GlobalMouseConfiguration& configuration);
    [[nodiscard]] std::optional<GlobalMouseDragEvent> interrupt();
    void resynchronize(const MacGlobalMouseInputState& state);
    void reset();
};

[[nodiscard]] MacGlobalMouseApi nativeMacGlobalMouseApi();
[[nodiscard]] std::unique_ptr<GlobalMouseBackend>
createMacGlobalMouseBackend(MacGlobalMouseApi api);
} // namespace snow_shot::presentation::detail

namespace snow_shot::presentation {
[[nodiscard]] std::unique_ptr<GlobalMouseBackend> createMacOSGlobalMouseBackend();
}
#endif
