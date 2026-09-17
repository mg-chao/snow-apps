#include "globalmousebackend_p.h"

#include <IOKit/hidsystem/IOLLEvent.h>
#include <cmath>
#include <limits>
#include <utility>

namespace snow_shot::presentation::detail {
namespace {
constexpr CGEventFlags activationFlags = kCGEventFlagMaskCommand | kCGEventFlagMaskControl |
                                         kCGEventFlagMaskAlternate | kCGEventFlagMaskShift;
CGEventFlags nativeModifierMask(CGEventFlags flags) {
    // Remove the device-specific left/right bits as well as the aggregate flags.
    // Otherwise applications inspecting NSEvent's raw flags can still see the chord.
    if (flags & kCGEventFlagMaskCommand)
        flags |= NX_DEVICELCMDKEYMASK | NX_DEVICERCMDKEYMASK;
    if (flags & kCGEventFlagMaskControl)
        flags |= NX_DEVICELCTLKEYMASK | NX_DEVICERCTLKEYMASK;
    if (flags & kCGEventFlagMaskAlternate)
        flags |= NX_DEVICELALTKEYMASK | NX_DEVICERALTKEYMASK;
    if (flags & kCGEventFlagMaskShift)
        flags |= NX_DEVICELSHIFTKEYMASK | NX_DEVICERSHIFTKEYMASK;
    return flags;
}
Qt::MouseButton buttonForEvent(CGEventType type, CGEventRef event) {
    if (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp ||
        type == kCGEventLeftMouseDragged)
        return Qt::LeftButton;
    if (type == kCGEventRightMouseDown || type == kCGEventRightMouseUp ||
        type == kCGEventRightMouseDragged)
        return Qt::RightButton;
    if (type != kCGEventOtherMouseDown && type != kCGEventOtherMouseUp &&
        type != kCGEventOtherMouseDragged)
        return Qt::NoButton;
    const auto button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
    // Preserve ownership even for auxiliary buttons with no configurable action.
    if (button >= 2 && button <= 26)
        return static_cast<Qt::MouseButton>(quint32{1} << static_cast<unsigned>(button));
    return Qt::NoButton;
}
GlobalMouseModifiers modifiersForFlags(CGEventFlags flags) {
    GlobalMouseModifiers result;
    if (flags & kCGEventFlagMaskCommand)
        result |= GlobalMouseModifier::Command;
    if (flags & kCGEventFlagMaskControl)
        result |= GlobalMouseModifier::Control;
    if (flags & kCGEventFlagMaskAlternate)
        result |= GlobalMouseModifier::Alt;
    if (flags & kCGEventFlagMaskShift)
        result |= GlobalMouseModifier::Shift;
    return result;
}
} // namespace

GlobalMouseInputResult MacGlobalMouseInput::handle(CGEventType type, CGEventRef event,
                                                   const GlobalMouseConfiguration& configuration) {
    if (!event)
        return {};
    // Public Quartz events identify their posting process. WindowServer/HID
    // events use zero; do not let automation acquire or release physical ownership.
    if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0)
        return {};
    const CGEventFlags flags = CGEventGetFlags(event);
    // Retire only modifiers physically released; masking survives Finish/Cancel
    // and never steals a later independent press of the same modifier.
    maskedFlags &= flags;
    if (type == kCGEventKeyDown || type == kCGEventKeyUp || type == kCGEventFlagsChanged) {
        GlobalMouseInputResult result;
        if (type != kCGEventFlagsChanged &&
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) == 53) {
            if (type == kCGEventKeyUp) {
                result.consumed = std::exchange(escapeConsumed, false);
            } else if (escapeConsumed) {
                result.consumed = true;
            } else if (gesture.pending()) {
                result =
                    gesture.handle({GlobalMouseInput::Kind::Cancel, lastPosition}, configuration);
                escapeConsumed = result.consumed;
            }
        }
        // Preserve physical modifier transitions (including both sides), balancing
        // the presses delivered before ownership. Actual key/pointer events lose
        // the activation chord; a flagsChanged event must not impersonate a release.
        if (type != kCGEventFlagsChanged)
            CGEventSetFlags(event, flags & ~nativeModifierMask(maskedFlags));
        return result;
    }

    if (type == kCGEventScrollWheel) {
        CGEventSetFlags(event, flags & ~nativeModifierMask(maskedFlags));
        return {};
    }
    GlobalMouseInput input;
    const bool dragged = type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDragged ||
                         type == kCGEventOtherMouseDragged;
    if (dragged || type == kCGEventMouseMoved)
        input.kind = GlobalMouseInput::Kind::Move;
    else if (type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown ||
             type == kCGEventOtherMouseDown)
        input.kind = GlobalMouseInput::Kind::Press;
    else if (type == kCGEventLeftMouseUp || type == kCGEventRightMouseUp ||
             type == kCGEventOtherMouseUp)
        input.kind = GlobalMouseInput::Kind::Release;
    else
        return {};
    const CGPoint point = CGEventGetLocation(event);
    const bool validPosition = std::isfinite(point.x) && std::isfinite(point.y) &&
                               std::abs(point.x) <= std::numeric_limits<int>::max() - 1.0 &&
                               std::abs(point.y) <= std::numeric_limits<int>::max() - 1.0;
    // Quartz desktop points and Qt Cocoa global logical coordinates share a
    // top-left origin. Never multiply by DPR or query QScreen on the tap thread.
    if (validPosition)
        lastPosition = QPointF(point.x, point.y);
    input.position = lastPosition;
    input.button = buttonForEvent(type, event);
    input.modifiers = modifiersForFlags(flags);
    input.heldButtons = heldButtons;
    if (input.kind == GlobalMouseInput::Kind::Press && input.button != Qt::NoButton &&
        !gesture.active() && !gesture.ownsButton(input.button) &&
        heldButtons.testFlag(input.button)) {
        CGEventSetFlags(event, flags & ~nativeModifierMask(maskedFlags));
        return {};
    }
    const bool ownedMotion = gesture.ownsButton(input.button);
    auto result = gesture.handle(input, configuration);
    if (input.kind == GlobalMouseInput::Kind::Press)
        heldButtons |= input.button;
    if (input.kind == GlobalMouseInput::Kind::Release)
        heldButtons &= ~Qt::MouseButtons(input.button);
    if (result.event && result.event->kind == GlobalMouseDragEvent::Kind::Begin)
        maskedFlags = flags & activationFlags;
    if (!result.consumed) {
        CGEventSetFlags(event, flags & ~nativeModifierMask(maskedFlags));
        // macOS represents dragging as a different event type. An application
        // whose press we swallowed must receive ordinary cursor movement.
        if (dragged && ownedMotion)
            CGEventSetType(event, kCGEventMouseMoved);
    }
    return result;
}

std::optional<GlobalMouseDragEvent> MacGlobalMouseInput::interrupt() {
    return gesture.handle({GlobalMouseInput::Kind::Cancel, lastPosition}, {}).event;
}
void MacGlobalMouseInput::resynchronize(Qt::MouseButtons buttons, CGEventFlags flags,
                                        bool escapeHeld) {
    // A timeout can hide releases. Retire only those pairs known to have ended;
    // still-held swallowed presses remain owned until their real releases arrive.
    const auto released = heldButtons & ~buttons;
    for (unsigned i = 0; i < 27; ++i) {
        const auto button = static_cast<Qt::MouseButton>(quint32{1} << i);
        if (released.testFlag(button))
            static_cast<void>(
                gesture.handle({GlobalMouseInput::Kind::Release, lastPosition, button}, {}));
    }
    heldButtons = buttons;
    maskedFlags &= flags;
    escapeConsumed = escapeConsumed && escapeHeld;
}
void MacGlobalMouseInput::reset() {
    gesture.reset();
    heldButtons = {};
    maskedFlags = 0;
    escapeConsumed = false;
}
} // namespace snow_shot::presentation::detail
