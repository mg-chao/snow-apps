#include "../src/platform/macos/globalmousebackend_p.h"
#include "snow_shot/platform/macos/globalmousebackend.h"
#include "snow_shot/platform/macos/modifierkeys.h"
#include "snow_shot/presentation/shortcutdisplaytext.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QGuiApplication>
#include <QTemporaryDir>
#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
namespace macos = snow_shot::platform::macos;
namespace presentation = snow_shot::presentation;
using Action = presentation::settings::SettingsGlobalMouseAction;
using EventKind = presentation::GlobalMouseDragEvent::Kind;
using Processor = macos::detail::GlobalMouseEventProcessor;
using Input = macos::detail::NativeMouseInput;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
Processor processor(Action action = Action::ScreenshotCopy, QString key = QStringLiteral("windows"),
                    QString button = QStringLiteral("left_drag")) {
    Processor result;
    const auto binding = presentation::globalMouseBinding(action, {{key}, button});
    require(binding.has_value(), "every exposed mouse combination must map to a binding");
    result.configure({{*binding}, true});
    result.setDisplays({{{0, 0, 1800, 1169}, 2}, {{-656, -1800, 3200, 1800}, 2}});
    return result;
}
Input input(CGEventType type, QPointF point = {50, 60},
            CGEventFlags flags = kCGEventFlagMaskCommand, int button = 0) {
    Input value;
    value.type = type;
    value.position = point;
    value.flags = flags;
    value.buttonNumber = button;
    return value;
}
void modifiersAndDisplayLabelsAgree() {
    for (const bool dontSwap : {false, true}) {
        QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, dontSwap);
        require(presentation::formatShortcutDisplayText(QStringLiteral("Ctrl+Alt+S")) ==
                    (dontSwap ? QStringLiteral("Control+Option+S")
                              : QStringLiteral("Command+Option+S")),
                "Ctrl display must follow Qt's physical Command/Control mapping");
        require(
            presentation::formatShortcutDisplayText(QStringLiteral("Meta+Shift+S")) ==
                (dontSwap ? QStringLiteral("Command+Shift+S") : QStringLiteral("Control+Shift+S")),
            "Meta display must follow Qt's physical Control/Command mapping");
        require(presentation::formatShortcutDisplayText(QStringLiteral("Ctrl++")) ==
                    (dontSwap ? QStringLiteral("Control+Plus") : QStringLiteral("Command+Plus")),
                "platform key labels must retain plus-key normalization");
        for (const auto& [key, flags] :
             {std::pair{QStringLiteral("windows"), kCGEventFlagMaskCommand},
              std::pair{QStringLiteral("ctrl"), kCGEventFlagMaskControl},
              std::pair{QStringLiteral("alt"), kCGEventFlagMaskAlternate},
              std::pair{QStringLiteral("shift"), kCGEventFlagMaskShift}}) {
            auto events = processor(Action::ScreenshotCopy, key);
            const auto result = events.handle(input(kCGEventLeftMouseDown, {50, 60}, flags));
            require(result.input.consumed && result.input.event &&
                        result.input.event->kind == EventKind::Begin,
                    "stored physical modifier names must match the native event under both Qt swap "
                    "modes");
        }
    }
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, false);
}
void allButtonsAndActionsSuppressTheOwnedDrag() {
    struct Button {
        const char* key;
        CGEventType down;
        CGEventType move;
        CGEventType up;
        int number;
    };
    const std::array buttons{Button{"left_drag", kCGEventLeftMouseDown, kCGEventLeftMouseDragged,
                                    kCGEventLeftMouseUp, 0},
                             Button{"right_drag", kCGEventRightMouseDown, kCGEventRightMouseDragged,
                                    kCGEventRightMouseUp, 1},
                             Button{"wheel_drag", kCGEventOtherMouseDown, kCGEventOtherMouseDragged,
                                    kCGEventOtherMouseUp, 2},
                             Button{"side_button_1_drag", kCGEventOtherMouseDown,
                                    kCGEventOtherMouseDragged, kCGEventOtherMouseUp, 3},
                             Button{"side_button_2_drag", kCGEventOtherMouseDown,
                                    kCGEventOtherMouseDragged, kCGEventOtherMouseUp, 4}};
    for (const auto action :
         {Action::ScreenshotCopy, Action::ScreenshotFixed, Action::ScreenshotOcr,
          Action::ScreenshotTranslation, Action::ScreenshotSave, Action::ScreenshotQuickSave,
          Action::ScreenRecording}) {
        for (const auto& button : buttons) {
            auto events =
                processor(action, QStringLiteral("windows"), QString::fromLatin1(button.key));
            auto unrelated = input(button.down, {50, 60},
                                   kCGEventFlagMaskCommand | kCGEventFlagMaskShift, button.number);
            require(!events.handle(unrelated).input.event,
                    "additional modifiers must not start a gesture");
            unrelated.flags = kCGEventFlagMaskCommand;
            unrelated.heldButtons = Qt::LeftButton | Qt::RightButton;
            require(!events.handle(unrelated).input.event,
                    "an existing foreign button drag must not be stolen");
            auto injected = input(button.down, {50, 60}, kCGEventFlagMaskCommand, button.number);
            injected.injected = true;
            require(!events.handle(injected).input.consumed,
                    "injected mouse events must pass without activation");
            const auto begin =
                events.handle(input(button.down, {50, 60}, kCGEventFlagMaskCommand, button.number));
            require(begin.input.consumed && begin.input.event &&
                        begin.input.event->action == action &&
                        begin.input.event->position == QPoint(100, 120),
                    "press must latch the action and physical anchor");
            const auto move = events.handle(input(button.move, {-100, -100}, 0, button.number));
            require(
                !move.input.consumed && move.normalizeDraggedMove && move.input.event &&
                    move.input.event->kind == EventKind::Update &&
                    move.input.event->position == QPoint(-200, -200),
                "owned native dragged events must become ordinary motion while crossing displays");
            const auto finish = events.handle(input(button.up, {-90, -90}, 0, button.number));
            require(finish.input.consumed && finish.input.event &&
                        finish.input.event->kind == EventKind::Finish &&
                        finish.input.event->id == begin.input.event->id,
                    "release must be swallowed once with the final position");
            require(!events.handle(input(button.up, {-90, -90}, 0, button.number)).input.event,
                    "duplicate releases must not confirm twice");
            const auto pendingCancel = events.cancelPending();
            require(pendingCancel.event && pendingCancel.event->kind == EventKind::Cancel,
                    "cancellation must reach a released gesture still preparing its capture");
        }
    }
}
void cancellationRetainsInputPairs() {
    auto events = processor();
    const auto begin = events.handle(input(kCGEventLeftMouseDown));
    const quint64 id = begin.input.event->id;
    Input escape = input(kCGEventKeyDown);
    escape.keyCode = 53;
    auto cancel = events.handle(escape);
    require(cancel.input.consumed && cancel.input.event &&
                cancel.input.event->kind == EventKind::Cancel,
            "Escape must cancel the gesture and suppress its key press");
    const auto remainingDrag = events.handle(input(kCGEventLeftMouseDragged));
    require(remainingDrag.normalizeDraggedMove && !remainingDrag.input.event,
            "cancelled swallowed presses must keep their remaining drag out of foreground apps");
    require(!events.handle(input(kCGEventRightMouseDragged)).normalizeDraggedMove,
            "cancellation must not alter a different button's unowned drag");
    require(events.handle(escape).input.consumed,
            "Escape autorepeat must remain consumed after cancellation");
    escape.type = kCGEventKeyUp;
    require(events.handle(escape).input.consumed && !events.handle(escape).input.consumed,
            "only the matching Escape release must be consumed");
    require(events.handle(input(kCGEventLeftMouseUp)).input.consumed,
            "the original mouse release must still be drained after Escape");
    require(!events.handle(input(kCGEventLeftMouseDragged)).normalizeDraggedMove,
            "release must end normalization for the swallowed button");
    const auto next = events.handle(input(kCGEventLeftMouseDown));
    require(next.input.event && next.input.event->id > id,
            "later gestures must have distinct ownership IDs");
    events.cancel(id);
    require(events.active(), "a stale cancellation must not cancel a newer gesture");
    cancel.input = events.cancelInterruptedInput(Qt::LeftButton, false);
    require(cancel.input.event && !events.pending(),
            "tap timeout cancellation must terminate capture ownership");
    require(events.handle(input(kCGEventLeftMouseDragged)).normalizeDraggedMove,
            "tap timeout recovery must retain ownership of remaining native drag events");
    require(events.handle(input(kCGEventLeftMouseUp)).input.consumed,
            "tap recovery must drain the release belonging to its swallowed press");
    auto ordinary = input(kCGEventKeyDown);
    ordinary.keyCode = 0;
    require(!events.handle(ordinary).input.consumed &&
                !events.handle(input(kCGEventFlagsChanged)).input.consumed,
            "ordinary keys and modifier changes must remain untouched");
}
void tapRecoveryReconcilesReleasesLostWhileDisabled() {
    for (const bool cancelledWithEscape : {false, true}) {
        auto events = processor();
        const auto begin = events.handle(input(kCGEventLeftMouseDown));
        auto escape = input(kCGEventKeyDown);
        escape.keyCode = 53;
        if (cancelledWithEscape)
            static_cast<void>(events.handle(escape));
        const auto cancelled = events.cancelInterruptedInput({}, false);
        require(cancelled.event.has_value() != cancelledWithEscape && !events.pending(),
                "a tap interruption must cancel the pending capture exactly once");
        const auto next = events.handle(input(kCGEventLeftMouseDown));
        require(next.input.event && next.input.event->id > begin.input.event->id,
                "a release lost during interruption must not block the next gesture");
        escape.type = kCGEventKeyUp;
        require(!events.handle(escape).input.consumed,
                "an Escape release lost during interruption must not consume a later release");
    }
}
void directButtonDragBalancesQtAndQuickRelease() {
    auto events = processor(Action::ScreenshotOcr);
    const auto begin = events.beginButtonDrag(Action::ScreenshotFixed, {100, 150});
    require(
        begin.event && begin.event->action == Action::ScreenshotFixed && !begin.consumed,
        "a settings-row press must start its own action without swallowing Qt's existing press");
    const auto move = events.handle(input(kCGEventLeftMouseDragged, {101, 152}, 0));
    require(move.input.event && !move.input.consumed && !move.normalizeDraggedMove,
            "the row's original Qt button press must retain native dragged events");
    const auto finish = events.handle(input(kCGEventLeftMouseUp, {102, 153}, 0));
    require(finish.input.event && finish.input.event->kind == EventKind::Finish &&
                !finish.input.consumed,
            "a queued fast release must reach both capture and the Qt widget");
    require(!events.beginButtonDrag(Action::ScreenshotOcr, {10, 10}).event,
            "a second row drag must wait until the pending capture is acknowledged");
    events.cancel(begin.event->id);
    require(events.beginButtonDrag(Action::ScreenshotOcr, {10, 10}).event.has_value(),
            "a completed capture must release the row for the next drag");
}
void mixedScaleCoordinatesMatchCaptureSpace() {
    auto events = processor();
    events.setDisplays(
        {{{0, 0, 1800, 1169}, 2}, {{1800, 0, 1920, 1080}, 1}, {{-1920, 0, 1920, 1080}, 1}});
    require(events.physicalPosition({100, 100}) == QPoint(200, 200),
            "Retina local offsets must scale by two");
    require(events.physicalPosition({1800, 100}) == QPoint(3600, 100) &&
                events.physicalPosition({1810, 100}) == QPoint(3610, 100),
            "a 1x display must retain its local pixels after the shared desktop origin transform");
    require(events.physicalPosition({-1910, 100}) == QPoint(-3830, 100),
            "negative mixed-DPI origins must use the same non-overlapping capture coordinates");
}
class RetryBackend final : public presentation::GlobalMouseBackend {
  public:
    int starts = 0;
    int stops = 0;
    presentation::GlobalMouseConfiguration configuration;
    FailureHandler firstFailure;
    void start(Handler, FailureHandler failure) override {
        ++starts;
        if (starts == 1) {
            firstFailure = failure;
            failure(static_cast<quint32>(macos::GlobalMouseError::AccessibilityPermission));
        }
    }
    void stop() override {
        ++stops;
    }
    void configure(const presentation::GlobalMouseConfiguration& value) override {
        configuration = value;
    }
    void cancel(quint64) override {}
    void beginButtonDrag(Action) override {}
};
void permissionFailureCanBeRetriedWithoutRestart() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "permission retry test storage must initialize");
    {
        auto native = std::make_unique<RetryBackend>();
        auto* inputBackend = native.get();
        presentation::GlobalMouseManager manager(std::move(native));
        int permissionRequests = 0;
        QObject::connect(
            &manager, &presentation::GlobalMouseManager::inputPermissionRequired,
            [&permissionRequests](const QString& text) {
                require(text.contains(QStringLiteral("Privacy & Security > Accessibility")),
                        "the permission error must tell the user where to enable access");
                ++permissionRequests;
            });
        manager.setCaptureAvailable(true);
        manager.initialize();
        QCoreApplication::processEvents();
        require(permissionRequests == 1, "permission failure must reach actionable UI");
        manager.retryInitialization();
        inputBackend->firstFailure(
            static_cast<quint32>(macos::GlobalMouseError::AccessibilityPermission));
        QCoreApplication::processEvents();
        require(inputBackend->starts == 2 && inputBackend->stops == 1 &&
                    inputBackend->configuration.captureAvailable && permissionRequests == 1,
                "retry must restart input with existing capture configuration and reject stale "
                "failures");
    }
    storage.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    modifiersAndDisplayLabelsAgree();
    allButtonsAndActionsSuppressTheOwnedDrag();
    cancellationRetainsInputPairs();
    tapRecoveryReconcilesReleasesLostWhileDisabled();
    directButtonDragBalancesQtAndQuickRelease();
    mixedScaleCoordinatesMatchCaptureSpace();
    permissionFailureCanBeRetriedWithoutRestart();
    std::cout
        << "macOS global mouse event ownership, coordinates, modifier labels and retry passed\n";
}
