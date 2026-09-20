#include "../src/platform/macos/globalmousebackend_p.h"

#include <IOKit/hidsystem/IOLLEvent.h>
#include <QApplication>
#include <QSemaphore>
#include <QThread>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

namespace {
using namespace snow_shot::presentation;
using Status = GlobalMousePermissionState::Status;
using Kind = GlobalMouseDragEvent::Kind;
using Action = settings::SettingsGlobalMouseAction;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct Event {
    CGEventRef value = CGEventCreate(nullptr);
    Event(CGEventType type, CGEventFlags flags = kCGEventFlagMaskCommand, int button = 0,
          CGPoint point = {-123.0, 57.0}) {
        require(value != nullptr, "private event allocation");
        CGEventSetType(value, type);
        CGEventSetFlags(value, flags);
        CGEventSetIntegerValueField(value, kCGMouseEventButtonNumber, button);
        CGEventSetIntegerValueField(value, kCGEventSourceUnixProcessID, 0);
        CGEventSetLocation(value, point);
    }
    ~Event() {
        CFRelease(value);
    }
};
GlobalMouseConfiguration config(Qt::MouseButton button = Qt::LeftButton,
                                GlobalMouseModifiers modifiers = GlobalMouseModifier::Command) {
    return {{{Action::ScreenshotCopy, modifiers, button}}, true};
}
void inputMappingAndOwnership() {
    constexpr std::array flags{kCGEventFlagMaskControl, kCGEventFlagMaskAlternate,
                               kCGEventFlagMaskShift, kCGEventFlagMaskCommand};
    constexpr std::array modifiers{GlobalMouseModifier::Control, GlobalMouseModifier::Alt,
                                   GlobalMouseModifier::Shift, GlobalMouseModifier::Command};
    constexpr std::array buttons{Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton,
                                 Qt::ForwardButton};
    for (unsigned mask = 1; mask < 16; ++mask) {
        CGEventFlags chord = 0;
        GlobalMouseModifiers domainChord;
        for (unsigned i = 0; i < flags.size(); ++i) {
            if ((mask & (1U << i)) != 0) {
                chord |= flags[i];
                domainChord |= modifiers[i];
            }
        }
        for (unsigned i = 0; i < buttons.size(); ++i) {
            const auto downType = i == 0   ? kCGEventLeftMouseDown
                                  : i == 1 ? kCGEventRightMouseDown
                                           : kCGEventOtherMouseDown;
            const auto upType = i == 0   ? kCGEventLeftMouseUp
                                : i == 1 ? kCGEventRightMouseUp
                                         : kCGEventOtherMouseUp;
            const auto moveType = i == 0   ? kCGEventLeftMouseDragged
                                  : i == 1 ? kCGEventRightMouseDragged
                                           : kCGEventOtherMouseDragged;
            detail::MacGlobalMouseInput input;
            const auto configuration = config(buttons[i], domainChord);
            Event down(downType, chord | kCGEventFlagMaskAlphaShift, static_cast<int>(i));
            auto begin = input.handle(downType, down.value, configuration);
            require(begin.consumed && begin.event && begin.event->position == QPoint(-123, 57),
                    "physical modifier chord and all buttons map exactly; Caps Lock is ignored");
            Event key(kCGEventKeyDown, chord | kCGEventFlagMaskAlphaShift);
            CGEventSetIntegerValueField(key.value, kCGKeyboardEventKeycode, 0);
            require(!input.handle(kCGEventKeyDown, key.value, configuration).consumed &&
                        CGEventGetFlags(key.value) == kCGEventFlagMaskAlphaShift,
                    "unrelated keys pass with only the initiating modifiers removed");
            Event move(moveType, chord, static_cast<int>(i), {-42.5, -91.5});
            auto update = input.handle(moveType, move.value, {});
            require(update.event && update.event->position == QPointF(-42.5, -91.5) &&
                        !update.consumed && CGEventGetType(move.value) == kCGEventMouseMoved,
                    "claimed dragged events become cursor moves with exact desktop points");
            Event up(upType, chord, static_cast<int>(i), {333.5, 901.5});
            auto finish = input.handle(upType, up.value, {});
            require(finish.consumed && finish.event && finish.event->kind == Kind::Finish &&
                        finish.event->id == begin.event->id &&
                        finish.event->position == QPointF(333.5, 901.5),
                    "release is exact even after a settings change");
            Event releasedModifier(kCGEventFlagsChanged, 0);
            require(!input.handle(kCGEventFlagsChanged, releasedModifier.value, {}).consumed &&
                        input.maskedFlags == 0,
                    "modifier release is balanced after finish");
            require(!input.handle(upType, up.value, {}).event, "duplicate release has no action");
        }
    }
    detail::MacGlobalMouseInput input;
    Event down(kCGEventLeftMouseDown);
    CGEventSetIntegerValueField(down.value, kCGEventSourceUnixProcessID, 1234);
    require(input.handle(kCGEventLeftMouseDown, down.value, config()).event.has_value(),
            "forwarded session input must be able to start a configured gesture");
    input.reset();
    CGEventSetIntegerValueField(down.value, kCGEventSourceUnixProcessID, 0);
    input.heldButtons = Qt::RightButton;
    require(!input.handle(kCGEventLeftMouseDown, down.value, config()).consumed,
            "existing application drag must not be stolen");
    input.heldButtons = Qt::LeftButton;
    require(!input.handle(kCGEventLeftMouseDown, down.value, config()).consumed,
            "a duplicate press of a button held before tap installation must not start capture");
    input.reset();
    auto begin = input.handle(kCGEventLeftMouseDown, down.value, config());
    require(begin.event.has_value(), "valid native begin");
    Event modifier(kCGEventFlagsChanged,
                   kCGEventFlagMaskCommand | NX_DEVICELCMDKEYMASK | NX_DEVICERCMDKEYMASK);
    const auto rawModifiers = CGEventGetFlags(modifier.value);
    require(!input.handle(kCGEventFlagsChanged, modifier.value, config()).consumed &&
                CGEventGetFlags(modifier.value) == rawModifiers,
            "both modifier sides must retain balanced physical transitions");
    Event scroll(kCGEventScrollWheel, rawModifiers | kCGEventFlagMaskShift);
    require(!input.handle(kCGEventScrollWheel, scroll.value, config()).consumed &&
                CGEventGetFlags(scroll.value) == kCGEventFlagMaskShift,
            "scroll events pass with only owned aggregate and device flags removed");
    Event extra(kCGEventOtherMouseDown, kCGEventFlagMaskCommand, 4);
    require(input.handle(kCGEventOtherMouseDown, extra.value, config()).consumed,
            "extra physical press is swallowed");
    Event escape(kCGEventKeyDown);
    CGEventSetIntegerValueField(escape.value, kCGKeyboardEventKeycode, 53);
    auto cancel = input.handle(kCGEventKeyDown, escape.value, config());
    require(cancel.consumed && cancel.event && cancel.event->kind == Kind::Cancel,
            "Escape cancels an active capture");
    Event unrelatedUp(kCGEventKeyUp);
    CGEventSetIntegerValueField(unrelatedUp.value, kCGKeyboardEventKeycode, 0);
    require(!input.handle(kCGEventKeyUp, unrelatedUp.value, config()).consumed,
            "Escape ownership must never swallow another key's release");
    require(input.handle(kCGEventKeyDown, escape.value, config()).consumed,
            "Escape autorepeats stay consumed");
    CGEventSetType(escape.value, kCGEventKeyUp);
    require(input.handle(kCGEventKeyUp, escape.value, config()).consumed,
            "Escape release balances the consumed press");
    Event afterCancel(kCGEventRightMouseDown);
    require(input.handle(kCGEventRightMouseDown, afterCancel.value, {}).consumed,
            "additional presses while draining a cancelled gesture must remain owned");
    CGEventSetType(afterCancel.value, kCGEventRightMouseUp);
    require(input.handle(kCGEventRightMouseUp, afterCancel.value, {}).consumed,
            "post-cancellation additional releases remain balanced");
    Event release(kCGEventLeftMouseUp);
    require(input.handle(kCGEventLeftMouseUp, release.value, {}).consumed,
            "cancel drains the initiating release");
    CGEventSetType(extra.value, kCGEventOtherMouseUp);
    require(input.handle(kCGEventOtherMouseUp, extra.value, {}).consumed,
            "cancel also drains additional button releases");
    require(!input.interrupt(), "repeat interruption produces no duplicate terminal event");
    input.reset();
    Event lostDown(kCGEventLeftMouseDown);
    require(input.handle(kCGEventLeftMouseDown, lostDown.value, config()).event.has_value(),
            "gesture before dropped-release recovery");
    static_cast<void>(input.interrupt());
    input.resynchronize({});
    Event freshDown(kCGEventLeftMouseDown);
    require(input.handle(kCGEventLeftMouseDown, freshDown.value, config()).event.has_value(),
            "a release missed during timeout must not poison the next gesture");
    input.reset();
    require(input.gesture.beginButtonDrag(Action::ScreenshotCopy, {}).event.has_value(),
            "local button drag starts");
    Event extraDirect(kCGEventRightMouseDown);
    require(input.handle(kCGEventRightMouseDown, extraDirect.value, {}).consumed,
            "additional presses during a local Qt drag are claimed");
    Event move(kCGEventLeftMouseDragged);
    static_cast<void>(input.handle(kCGEventLeftMouseDragged, move.value, {}));
    require(CGEventGetType(move.value) == kCGEventLeftMouseDragged,
            "local Qt presses retain normal drag and release routing");
    require(!input.handle(kCGEventLeftMouseUp, release.value, {}).consumed,
            "local press must get its Qt release");
}

void forwardedSessionInput() {
    for (const auto source : {kCGEventSourceStatePrivate, kCGEventSourceStateCombinedSessionState,
                              kCGEventSourceStateHIDSystemState}) {
        detail::MacGlobalMouseInput input;
        const auto forwarded = [source](Event& event) {
            CGEventSetIntegerValueField(event.value, kCGEventSourceUnixProcessID, 1234);
            CGEventSetIntegerValueField(event.value, kCGEventSourceStateID, source);
        };
        Event down(kCGEventLeftMouseDown);
        forwarded(down);
        const auto begin = input.handle(kCGEventLeftMouseDown, down.value, config());
        require(begin.consumed && begin.event && begin.event->kind == Kind::Begin,
                "forwarded modifier + mouse press starts a gesture regardless of source state");
        Event move(kCGEventLeftMouseDragged, kCGEventFlagMaskCommand, 0, {100, 120});
        forwarded(move);
        const auto update = input.handle(kCGEventLeftMouseDragged, move.value, {});
        require(update.event && update.event->id == begin.event->id && !update.consumed &&
                    CGEventGetType(move.value) == kCGEventMouseMoved &&
                    CGEventGetFlags(move.value) == 0,
                "forwarded drag updates selection and preserves unmodified cursor movement");
        Event up(kCGEventLeftMouseUp, 0, 0, {150, 160});
        forwarded(up);
        const auto finish = input.handle(kCGEventLeftMouseUp, up.value, {});
        require(finish.consumed && finish.event && finish.event->kind == Kind::Finish &&
                    finish.event->id == begin.event->id && !input.gesture.active(),
                "forwarded release completes the selection instead of leaving capture active");
        Event escape(kCGEventKeyDown, 0);
        forwarded(escape);
        CGEventSetIntegerValueField(escape.value, kCGKeyboardEventKeycode, 53);
        const auto cancel = input.handle(kCGEventKeyDown, escape.value, {});
        require(cancel.consumed && cancel.event && cancel.event->kind == Kind::Cancel,
                "forwarded Escape can cancel capture still preparing after release");
        input.reset();
        require(input.gesture.beginButtonDrag(Action::ScreenshotCopy, {}).event.has_value(),
                "GUI button drag begins before forwarded native input arrives");
        Event directMove(kCGEventLeftMouseDragged, 0, 0, {100, 120});
        forwarded(directMove);
        const auto directUpdate = input.handle(kCGEventLeftMouseDragged, directMove.value, {});
        require(directUpdate.event && directUpdate.event->kind == Kind::Update &&
                    !directUpdate.consumed &&
                    CGEventGetType(directMove.value) == kCGEventLeftMouseDragged,
                "GUI-started drag receives forwarded movement and keeps Qt drag routing");
        const auto directFinish = input.handle(kCGEventLeftMouseUp, up.value, {});
        require(directFinish.event && directFinish.event->kind == Kind::Finish &&
                    !directFinish.consumed && !input.gesture.active(),
                "GUI-started drag receives forwarded release and lets Qt balance its press");
        input.reset();
        const auto unmatched = input.handle(kCGEventLeftMouseDown, down.value, {});
        require(!unmatched.consumed && !unmatched.event &&
                    CGEventGetType(down.value) == kCGEventLeftMouseDown &&
                    CGEventGetFlags(down.value) == kCGEventFlagMaskCommand,
                "ordinary screenshot input passes through when global capture is unavailable");
        input.reset();
        const auto recoveryBegin = input.handle(kCGEventLeftMouseDown, down.value, config());
        require(recoveryBegin.event && input.interrupt(),
                "forwarded gesture is cancelled when the session tap is interrupted");
        input.resynchronize({Qt::LeftButton, kCGEventFlagMaskCommand, false});
        const auto recoveryRelease = input.handle(kCGEventLeftMouseUp, up.value, {});
        require(recoveryRelease.consumed && !recoveryRelease.event && !input.gesture.active(),
                "combined-session recovery preserves a forwarded press until its release");
        const auto nextBegin = input.handle(kCGEventLeftMouseDown, down.value, config());
        require(nextBegin.event && input.interrupt(),
                "forwarded input remains usable after a recovered release");
        input.resynchronize({});
        const auto afterLostRelease = input.handle(kCGEventLeftMouseDown, down.value, config());
        require(afterLostRelease.event.has_value(),
                "combined-session recovery retires a forwarded release missed during timeout");
    }
}

struct Fixture {
    std::atomic_bool listen{false}, trusted{false}, enabled{false}, creationFails{false},
        enableFails{false};
    std::atomic_int installs{0}, removals{0}, requests{0}, permissionQueries{0};
    std::atomic_int heldButtons{0};
    std::atomic_bool openedListen{false}, sessionActive{true};
    QSemaphore changed;
    std::mutex mutex;
    std::vector<GlobalMouseDragEvent> events;
    using Callback = CGEventRef (*)(CGEventTapProxy, CGEventType, CGEventRef, void*);
    Callback callback = nullptr;
    void* context = nullptr;
    CFRunLoopRef loop = nullptr;
    std::unique_ptr<GlobalMouseBackend> backend;
    Fixture() {
        detail::MacGlobalMouseApi api;
        api.listenAccess = [this] {
            ++permissionQueries;
            return listen.load();
        };
        api.accessibilityAccess = [this] {
            ++permissionQueries;
            return trusted.load();
        };
        api.createTap = [this](CGEventMask mask, CGEventTapCallBack handler, void* receiver) {
            require((mask & CGEventMaskBit(kCGEventOtherMouseDragged)) != 0,
                    "tap mask covers auxiliary drags");
            ++installs;
            if (creationFails)
                return static_cast<CFMachPortRef>(nullptr);
            callback = handler;
            context = receiver;
            loop = CFRunLoopGetCurrent();
            return CFMachPortCreate(
                nullptr, [](CFMachPortRef, void*, CFIndex, void*) {}, nullptr, nullptr);
        };
        api.enableTap = [this](CFMachPortRef, bool value) {
            if (!value)
                ++removals;
            enabled = value && !enableFails;
        };
        api.tapEnabled = [this](CFMachPortRef) { return enabled.load(); };
        api.inputState = [this] {
            return detail::MacGlobalMouseInputState{Qt::MouseButtons(heldButtons.load()),
                                                    kCGEventFlagMaskCommand, false};
        };
        api.cursor = [] { return std::optional<QPoint>(QPoint(-10, 20)); };
        api.sessionActive = [this] { return sessionActive.load(); };
        api.requestAccess = [this] { ++requests; };
        api.openSettings = [this](bool missingListen) { openedListen = missingListen; };
        backend = detail::createMacGlobalMouseBackend(std::move(api));
        backend->setStateHandler([this](GlobalMousePermissionState) { changed.release(); });
        backend->configure(config());
    }
    ~Fixture() {
        backend->stop();
    }
    void start() {
        backend->start(
            [this](GlobalMouseDragEvent event) {
                std::lock_guard lock(mutex);
                events.push_back(event);
            },
            [](quint32) { require(false, "structured states must describe native failures"); });
    }
    void await(Status status) {
        for (int i = 0; i < 20; ++i) {
            if (backend->permissionState().status == status)
                return;
            static_cast<void>(changed.tryAcquire(1, 500));
        }
        require(false, "backend state transition timed out");
    }
    void onWorker(std::function<void()> scenario) {
        QSemaphore completed;
        auto* done = &completed;
        CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, ^{
          scenario();
          done->release();
        });
        CFRunLoopWakeUp(loop);
        require(completed.tryAcquire(1, 5000), "worker scenario must complete");
    }
};
void sharedPermissionSnapshot() {
    Fixture f;
    std::atomic_int refreshes{0};
    f.backend->setPermissionRefreshHandler([&] { ++refreshes; });
    f.backend->usePermissionSnapshot(true, true);
    f.start();
    f.await(Status::Ready);
    require(f.permissionQueries == 0 && f.installs == 1,
            "shared permissions must avoid independent TCC queries");
    for (int i = 0; i < 100; ++i)
        f.backend->usePermissionSnapshot(true, true);
    f.onWorker([&] {
        Event down(kCGEventLeftMouseDown);
        f.callback(nullptr, kCGEventLeftMouseDown, down.value, f.context);
        f.enabled = false;
        f.callback(nullptr, kCGEventTapDisabledByTimeout, nullptr, f.context);
        require(f.enabled, "cached permissions must preserve timeout recovery");
    });
    require(refreshes == 1 && f.permissionQueries == 0 && f.installs == 1,
            "disabled tap requests central refresh without rebuilding a healthy tap");
    f.backend->usePermissionSnapshot(true, false);
    f.await(Status::AccessibilityRequired);
    require(f.removals > 0 && f.permissionQueries == 0,
            "central revocation retires the tap without new permission probes");
    {
        std::lock_guard lock(f.mutex);
        require(f.events.size() == 2 && f.events.back().kind == Kind::Cancel,
                "permission interruption cancels a gesture once");
    }
    f.backend->usePermissionSnapshot(true, true);
    f.await(Status::Ready);
    f.sessionActive = false;
    f.backend->refreshPermission();
    f.await(Status::Suspended);
    require(f.backend->permissionState().listenGranted &&
                f.backend->permissionState().accessibilityGranted,
            "session suspension must preserve granted permission state");
    f.sessionActive = true;
    f.backend->refreshPermission();
    f.await(Status::Ready);
    require(f.permissionQueries == 0, "session recovery also uses the shared snapshot");
}

void lifecycleAndRecovery() {
    Fixture f;
    f.start();
    f.await(Status::ListenRequired);
    require(f.installs == 0, "missing permission must not install a tap");
    f.backend->requestPermission();
    f.backend->openPermissionSettings();
    require(f.requests == 1 && f.openedListen,
            "permission actions are explicit and correctly targeted");
    f.listen = true;
    f.backend->refreshPermission();
    f.await(Status::AccessibilityRequired);
    f.backend->openPermissionSettings();
    require(!f.openedListen, "Accessibility is the next missing permission");
    f.trusted = true;
    f.creationFails = true;
    f.backend->refreshPermission();
    f.await(Status::Unavailable);
    require(!f.backend->permissionState().tapAvailable, "tap creation failure is not Ready");
    f.creationFails = false;
    f.backend->refreshPermission();
    f.await(Status::Ready);
    f.onWorker([&] {
        Event down(kCGEventLeftMouseDown);
        require(f.callback(nullptr, kCGEventLeftMouseDown, down.value, f.context) == nullptr,
                "worker tap callback consumes the configured press");
        f.heldButtons = Qt::LeftButton;
        f.enabled = false;
        f.callback(nullptr, kCGEventTapDisabledByTimeout, nullptr, f.context);
        require(f.enabled, "a null-event timeout notification re-enables the tap");
        f.heldButtons = 0;
        Event release(kCGEventLeftMouseUp);
        require(f.callback(nullptr, kCGEventLeftMouseUp, release.value, f.context) == nullptr,
                "timeout cancellation still drains the swallowed press");
    });
    {
        std::lock_guard lock(f.mutex);
        require(f.events.size() == 2 && f.events[0].kind == Kind::Begin &&
                    f.events[1].kind == Kind::Cancel &&
                    f.events[0].coordinateSpace == GlobalMouseCoordinateSpace::DesktopPoints,
                "timeout cancels once and publishes the explicit coordinate space");
    }
    f.onWorker([&] {
        Event down(kCGEventLeftMouseDown);
        f.callback(nullptr, kCGEventLeftMouseDown, down.value, f.context);
    });
    f.trusted = false;
    f.backend->refreshPermission();
    f.await(Status::AccessibilityRequired);
    require(f.removals > 0, "revocation retires the tap and cancels capture");
    f.trusted = true;
    f.backend->refreshPermission();
    f.await(Status::Ready);
    f.enableFails = true;
    f.enabled = false;
    f.backend->refreshPermission();
    f.await(Status::Unavailable);
    f.enableFails = false;
    f.backend->refreshPermission();
    f.await(Status::Ready);
    f.sessionActive = false;
    f.backend->refreshPermission();
    f.await(Status::Suspended);
    require(!f.enabled, "session deactivation must retire the tap");
    f.sessionActive = true;
    f.backend->refreshPermission();
    f.await(Status::Ready);
    f.backend->stop();
    const auto stoppedEvents = f.events.size();
    f.start();
    f.await(Status::Ready);
    f.backend->stop();
    require(f.events.size() == stoppedEvents, "restart must not revive stale gestures");
    // Immediate stop while thread startup is pending must not hang or leave a port.
    for (int i = 0; i < 5; ++i) {
        f.start();
        f.backend->stop();
    }
    require(!f.enabled, "all event tap resources retire on shutdown");
    f.start();
    f.await(Status::Ready);
    f.onWorker([&] { f.backend->stop(); });
    f.await(Status::Unknown);
    f.backend->stop();
    require(!f.enabled, "stopping from a worker callback must not deadlock or retain the tap");
}
int smoke() {
    auto backend = createGlobalMouseBackend();
    QSemaphore stateChanged;
    backend->setStateHandler([&](GlobalMousePermissionState) { stateChanged.release(); });
    backend->configure(config());
    backend->start([](GlobalMouseDragEvent) {}, [](quint32) {});
    require(stateChanged.tryAcquire(1, 5000), "real native tap must report startup status");
    const auto state = backend->permissionState();
    backend->stop();
    if (state.status == Status::ListenRequired || state.status == Status::AccessibilityRequired) {
        std::cout << globalMousePermissionMessage(state).toStdString() << '\n';
        return 77;
    }
    require(state.status == Status::Ready && state.tapAvailable,
            "real native tap must become ready");
    // Limit this smoke test to our tagged events, without changing their native
    // source metadata. Posted session input must traverse the production path.
    struct Probe {
        CGEventTapCallBack callback = nullptr;
        void* context = nullptr;
    } probe;
    constexpr int64_t marker = 0x534e4f574d4f5553;
    auto api = detail::nativeMacGlobalMouseApi();
    api.createTap = [&probe](CGEventMask mask, CGEventTapCallBack callback, void* context) {
        probe.callback = callback;
        probe.context = context;
        return CGEventTapCreate(
            kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask,
            [](CGEventTapProxy proxy, CGEventType type, CGEventRef event,
               void* info) -> CGEventRef {
                auto& p = *static_cast<Probe*>(info);
                if (!event || CGEventGetIntegerValueField(event, kCGEventSourceUserData) != marker)
                    return event;
                return p.callback(proxy, type, event, p.context);
            },
            &probe);
    };
    auto native = detail::createMacGlobalMouseBackend(std::move(api));
    QSemaphore ready;
    QSemaphore received;
    std::vector<GlobalMouseDragEvent> events;
    native->configure(config());
    native->setStateHandler([&](auto value) {
        if (value.status == Status::Ready)
            ready.release();
    });
    native->start(
        [&](auto value) {
            events.push_back(value);
            received.release();
        },
        [](quint32) {});
    require(ready.tryAcquire(1, 5000), "real test tap must become ready");
    CGEventRef sample = CGEventCreate(nullptr);
    require(sample != nullptr, "native cursor sample");
    const auto point = CGEventGetLocation(sample);
    CFRelease(sample);
    for (const auto type : {kCGEventLeftMouseDown, kCGEventLeftMouseDragged, kCGEventLeftMouseUp}) {
        Event event(type, kCGEventFlagMaskCommand, 0, point);
        CGEventSetIntegerValueField(event.value, kCGEventSourceUserData, marker);
        CGEventPost(kCGSessionEventTap, event.value);
        require(received.tryAcquire(1, 5000), "posted drag event must traverse the real event tap");
    }
    native->stop();
    require(events.size() >= 3 && events[0].kind == Kind::Begin && events[1].kind == Kind::Update &&
                events[2].kind == Kind::Finish && events[0].id == events[2].id,
            "real event tap must complete the synthetic drag lifecycle");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--native-hook-smoke")))
        return smoke();
    forwardedSessionInput();
    inputMappingAndOwnership();
    lifecycleAndRecovery();
    sharedPermissionSnapshot();
    return 0;
}
