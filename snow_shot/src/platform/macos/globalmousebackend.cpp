#include "snow_shot/platform/macos/globalmousebackend.h"
#include "snow_shot/platform/macos/modifierkeys.h"
#include "globalmousebackend_p.h"

#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace snow_shot::platform::macos {
namespace {
using namespace snow_shot::presentation;
constexpr quint16 ESCAPE_KEY_CODE = 53;

Qt::KeyboardModifiers modifiers(CGEventFlags flags) {
    Qt::KeyboardModifiers result;
    if ((flags & kCGEventFlagMaskCommand) != 0)
        result |= commandModifier();
    if ((flags & kCGEventFlagMaskControl) != 0)
        result |= controlModifier();
    if ((flags & kCGEventFlagMaskAlternate) != 0)
        result |= Qt::AltModifier;
    if ((flags & kCGEventFlagMaskShift) != 0)
        result |= Qt::ShiftModifier;
    return result;
}

Qt::MouseButton mouseButton(CGEventType type, int number) {
    switch (type) {
    case kCGEventLeftMouseDown:
    case kCGEventLeftMouseUp:
    case kCGEventLeftMouseDragged:
        return Qt::LeftButton;
    case kCGEventRightMouseDown:
    case kCGEventRightMouseUp:
    case kCGEventRightMouseDragged:
        return Qt::RightButton;
    default:
        return number == 2   ? Qt::MiddleButton
               : number == 3 ? Qt::BackButton
               : number == 4 ? Qt::ForwardButton
                             : Qt::NoButton;
    }
}

bool mouseDown(CGEventType type) {
    return type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown ||
           type == kCGEventOtherMouseDown;
}
bool mouseUp(CGEventType type) {
    return type == kCGEventLeftMouseUp || type == kCGEventRightMouseUp ||
           type == kCGEventOtherMouseUp;
}
bool dragged(CGEventType type) {
    return type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDragged ||
           type == kCGEventOtherMouseDragged;
}

QPointF pointerPosition() {
    CGEventRef event = CGEventCreate(nullptr);
    if (event == nullptr)
        return {};
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    return {point.x, point.y};
}
Qt::MouseButtons heldButtons() {
    Qt::MouseButtons result;
    constexpr std::array buttons{Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton,
                                 Qt::ForwardButton};
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                     static_cast<CGMouseButton>(i)))
            result |= buttons[i];
    }
    return result;
}

QVector<detail::MouseDisplayMapping> displayMappings() {
    std::array<CGDirectDisplayID, 64> ids{};
    uint32_t count = 0;
    QVector<detail::MouseDisplayMapping> displays;
    if (CGGetActiveDisplayList(static_cast<uint32_t>(ids.size()), ids.data(), &count) !=
        kCGErrorSuccess)
        return displays;
    for (uint32_t i = 0; i < count; ++i) {
        const CGRect bounds = CGDisplayBounds(ids[i]);
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
        qreal scale = 1;
        if (mode != nullptr) {
            const auto width = CGDisplayModeGetWidth(mode);
            if (width > 0)
                scale = static_cast<qreal>(CGDisplayModeGetPixelWidth(mode)) /
                        static_cast<qreal>(width);
            CGDisplayModeRelease(mode);
        }
        displays.push_back(
            {{bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height}, scale});
    }
    return displays;
}

class MacGlobalMouseBackend final : public GlobalMouseBackend {
  public:
    ~MacGlobalMouseBackend() override {
        stop();
    }

    void start(Handler handler, FailureHandler failure) override {
        if (m_running.load())
            return;
        if (m_thread.joinable())
            m_thread.join();
        m_handler = std::move(handler);
        m_failure = std::move(failure);
        if (!AXIsProcessTrusted()) {
            fail(GlobalMouseError::AccessibilityPermission);
            return;
        }
        m_stopping.store(false);
        m_running.store(true);
        m_thread = std::thread([this] { run(); });
    }
    void stop() override {
        m_stopping.store(true);
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
            m_thread.join();
        std::lock_guard lock(m_mutex);
        m_commands.clear();
    }
    void configure(const GlobalMouseConfiguration& configuration) override {
        std::lock_guard lock(m_mutex);
        m_configuration = configuration;
    }
    void cancel(quint64 id) override {
        enqueue([this, id] { m_processor.cancel(id); });
    }
    void beginButtonDrag(settings::SettingsGlobalMouseAction action) override {
        if (!m_running.load() ||
            !CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft))
            return;
        const QPointF start = pointerPosition();
        enqueue([this, action, start] {
            refreshDisplays();
            deliver(m_processor.beginButtonDrag(action, start));
            // The release can arrive before this queued request is processed.
            if (!CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                          kCGMouseButtonLeft)) {
                detail::NativeMouseInput release;
                release.type = kCGEventLeftMouseUp;
                release.position = pointerPosition();
                deliver(m_processor.handle(release).input);
            }
        });
    }

  private:
    void enqueue(std::function<void()> command) {
        if (!m_running.load())
            return;
        std::lock_guard lock(m_mutex);
        m_commands.push_back(std::move(command));
    }
    void drainCommands() {
        std::vector<std::function<void()>> commands;
        {
            std::lock_guard lock(m_mutex);
            m_processor.configure(m_configuration);
            commands.swap(m_commands);
        }
        for (auto& command : commands)
            command();
    }
    void refreshDisplays() {
        m_processor.setDisplays(displayMappings());
        m_lastDisplayRefresh = std::chrono::steady_clock::now();
    }
    void deliver(const GlobalMouseInputResult& result) {
        if (result.event && m_handler)
            m_handler(*result.event);
    }
    void fail(GlobalMouseError error) {
        if (m_failure)
            m_failure(static_cast<quint32>(error));
    }
    static CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* context) {
        auto& self = *static_cast<MacGlobalMouseBackend*>(context);
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            self.deliver(self.m_processor.cancelInterruptedInput(
                heldButtons(),
                CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, ESCAPE_KEY_CODE)));
            if (type == kCGEventTapDisabledByTimeout && AXIsProcessTrusted()) {
                CGEventTapEnable(self.m_tap, true);
            } else {
                self.fail(GlobalMouseError::EventTapDisabled);
                self.m_stopping.store(true);
            }
            return event;
        }
        if (event == nullptr || self.m_stopping.load())
            return event;
        if (type == kCGEventMouseMoved && !self.m_processor.active())
            return event;
        if (mouseDown(type) || (self.m_processor.active() &&
                                std::chrono::steady_clock::now() - self.m_lastDisplayRefresh >
                                    std::chrono::seconds(1)))
            self.refreshDisplays();
        const CGPoint position = CGEventGetLocation(event);
        detail::NativeMouseInput input;
        input.type = type;
        input.position = {position.x, position.y};
        input.flags = CGEventGetFlags(event);
        input.buttonNumber =
            static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber));
        input.keyCode =
            static_cast<quint16>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
        input.injected = CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0;
        if (mouseDown(type))
            input.heldButtons = heldButtons();
        self.drainCommands();
        const auto result = self.m_processor.handle(input);
        self.deliver(result.input);
        if (result.input.consumed)
            return nullptr;
        if (result.normalizeDraggedMove) {
            // Unlike WM_MOUSEMOVE, macOS dragged events carry button ownership downstream.
            // Preserve pointer motion without exposing the swallowed drag to another app.
            CGEventSetType(event, kCGEventMouseMoved);
        }
        return event;
    }
    void run() {
        const CGEventMask mask =
            CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
            CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged) |
            CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged) |
            CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kCGEventKeyDown) |
            CGEventMaskBit(kCGEventKeyUp);
        m_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionDefault, mask, callback, this);
        CFRunLoopSourceRef source =
            m_tap ? CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0) : nullptr;
        if (source == nullptr) {
            if (m_tap != nullptr) {
                CFRelease(m_tap);
                m_tap = nullptr;
            }
            fail(GlobalMouseError::EventTapUnavailable);
            m_running.store(false);
            return;
        }
        refreshDisplays();
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
        CGEventTapEnable(m_tap, true);
        while (!m_stopping.load()) {
            drainCommands();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
        deliver(m_processor.cancelPending());
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
        CFRunLoopSourceInvalidate(source);
        CFRelease(source);
        CFMachPortInvalidate(m_tap);
        CFRelease(m_tap);
        m_tap = nullptr;
        m_processor.reset();
        m_running.store(false);
    }
    std::mutex m_mutex;
    std::thread m_thread;
    std::atomic_bool m_running = false;
    std::atomic_bool m_stopping = false;
    GlobalMouseConfiguration m_configuration;
    std::vector<std::function<void()>> m_commands;
    detail::GlobalMouseEventProcessor m_processor;
    Handler m_handler;
    FailureHandler m_failure;
    CFMachPortRef m_tap = nullptr;
    std::chrono::steady_clock::time_point m_lastDisplayRefresh;
};
} // namespace

namespace detail {
void GlobalMouseEventProcessor::configure(
    const presentation::GlobalMouseConfiguration& configuration) {
    m_configuration = configuration;
}
void GlobalMouseEventProcessor::setDisplays(QVector<MouseDisplayMapping> displays) {
    m_displays = std::move(displays);
}
QPoint GlobalMouseEventProcessor::physicalPosition(const QPointF& point) const {
    qreal desktopScale = 1;
    const MouseDisplayMapping* display = nullptr;
    for (const auto& candidate : m_displays) {
        desktopScale = std::max(desktopScale, candidate.backingScale);
        const auto& bounds = candidate.logicalBounds;
        if (point.x() >= bounds.left() && point.x() < bounds.right() && point.y() >= bounds.top() &&
            point.y() < bounds.bottom())
            display = &candidate;
    }
    if (display == nullptr)
        return point.toPoint();
    const QPointF origin = display->logicalBounds.topLeft();
    const QPointF physical = origin * desktopScale + (point - origin) * display->backingScale;
    return {qRound(physical.x()), qRound(physical.y())};
}
NativeMouseResult GlobalMouseEventProcessor::handle(const NativeMouseInput& input) {
    NativeMouseResult result;
    if (input.injected)
        return result;
    if (input.type == kCGEventKeyDown || input.type == kCGEventKeyUp) {
        if (input.keyCode != ESCAPE_KEY_CODE)
            return result;
        if (input.type == kCGEventKeyUp) {
            result.input.consumed = std::exchange(m_escapeConsumed, false);
        } else if (m_gesture.pending()) {
            result.input = cancelPending();
            m_escapeConsumed = result.input.consumed;
        } else {
            result.input.consumed = m_escapeConsumed;
        }
        return result;
    }
    if (input.type == kCGEventFlagsChanged || input.type == kCGEventNull)
        return result;
    m_lastPosition = physicalPosition(input.position);
    presentation::GlobalMouseInput translated;
    translated.position = m_lastPosition;
    translated.button = mouseButton(input.type, input.buttonNumber);
    translated.modifiers = modifiers(input.flags);
    translated.heldButtons = input.heldButtons;
    translated.kind = mouseDown(input.type) ? presentation::GlobalMouseInput::Kind::Press
                      : mouseUp(input.type) ? presentation::GlobalMouseInput::Kind::Release
                      : (input.type == kCGEventMouseMoved || dragged(input.type))
                          ? presentation::GlobalMouseInput::Kind::Move
                          : presentation::GlobalMouseInput::Kind::Other;
    const bool swallowedPress = m_gesture.hasConsumedPress(translated.button);
    result.input = m_gesture.handle(translated, m_configuration);
    // Cancellation ends capture ownership before the physical button is released. Keep the
    // remaining drag private until its swallowed press is balanced, while preserving Qt's
    // normal press/drag/release sequence for direct settings-row drags.
    result.normalizeDraggedMove = swallowedPress && dragged(input.type) && !result.input.consumed;
    return result;
}
presentation::GlobalMouseInputResult
GlobalMouseEventProcessor::beginButtonDrag(presentation::settings::SettingsGlobalMouseAction action,
                                           const QPointF& desktopPoint) {
    if (!m_configuration.captureAvailable)
        return {};
    m_lastPosition = physicalPosition(desktopPoint);
    return m_gesture.beginButtonDrag(action, m_lastPosition);
}
void GlobalMouseEventProcessor::cancel(quint64 id) {
    m_gesture.cancel(id);
}
presentation::GlobalMouseInputResult GlobalMouseEventProcessor::cancelPending() {
    return m_gesture.handle({presentation::GlobalMouseInput::Kind::Cancel, m_lastPosition},
                            m_configuration);
}
presentation::GlobalMouseInputResult
GlobalMouseEventProcessor::cancelInterruptedInput(Qt::MouseButtons heldButtons, bool escapeHeld) {
    auto result = cancelPending();
    // Releases can be lost while the tap is disabled. Reconcile only our swallowed presses,
    // so recovery cannot leave a later gesture waiting for an already released button.
    for (const auto button :
         {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton}) {
        if (!heldButtons.testFlag(button) && m_gesture.hasConsumedPress(button)) {
            static_cast<void>(m_gesture.handle(
                {presentation::GlobalMouseInput::Kind::Release, m_lastPosition, button},
                m_configuration));
        }
    }
    if (!escapeHeld)
        m_escapeConsumed = false;
    return result;
}
void GlobalMouseEventProcessor::reset() {
    m_gesture.reset();
    m_escapeConsumed = false;
}
} // namespace detail
std::unique_ptr<presentation::GlobalMouseBackend> createGlobalMouseBackend() {
    return std::make_unique<MacGlobalMouseBackend>();
}
} // namespace snow_shot::platform::macos
