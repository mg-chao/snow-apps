#include "snow_shot/presentation/globalmousemanager.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/screenshotglobalmousedrag.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>

#ifdef Q_OS_WIN
void globalMouseNativeActivationKeyTests();
void globalMouseNativePerformanceTests();
void globalMouseNativeStopFromWorkerThreadTests();
#endif

namespace {
using namespace snow_shot::presentation;
using Action = settings::SettingsGlobalMouseAction;
using InputKind = GlobalMouseInput::Kind;
using EventKind = GlobalMouseDragEvent::Kind;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

GlobalMouseConfiguration configured(Qt::MouseButton button = Qt::LeftButton) {
    return {{{Action::ScreenshotCopy, Qt::ControlModifier, button}}, true};
}

void everyCombinationMatchesExactlyAndLatchesItsAction() {
    const std::array keys{QStringLiteral("windows"), QStringLiteral("ctrl"), QStringLiteral("alt"),
                          QStringLiteral("shift")};
    const std::array buttons{QStringLiteral("left_drag"), QStringLiteral("right_drag"),
                             QStringLiteral("wheel_drag"), QStringLiteral("side_button_1_drag"),
                             QStringLiteral("side_button_2_drag")};
    const std::array actions{Action::ScreenshotCopy,      Action::ScreenshotFixed,
                             Action::ScreenshotOcr,       Action::ScreenshotTranslation,
                             Action::ScreenshotQuickSave, Action::ScreenshotSave,
                             Action::ScreenRecording};
    for (const auto& key : keys) {
        for (const auto& button : buttons) {
            for (const auto action : actions) {
                const auto binding = globalMouseBinding(action, {{key}, button});
                require(binding.has_value(), "every settings option must have a native binding");
                GlobalMouseConfiguration config{{*binding}, true};
                GlobalMouseGesture gesture;
                GlobalMouseInput press{
                    InputKind::Press, {-200, 50}, binding->button, binding->modifiers};
                auto extra = press;
                extra.modifiers |= binding->modifiers == Qt::ControlModifier ? Qt::ShiftModifier
                                                                             : Qt::ControlModifier;
                require(!gesture.handle(extra, config).event, "extra modifiers must not activate");
                extra.modifiers = {};
                require(!gesture.handle(extra, config).event,
                        "a bare mouse press must pass through");
                const auto begin = gesture.handle(press, config);
                require(begin.consumed && begin.event && begin.event->kind == EventKind::Begin &&
                            begin.event->position == press.position &&
                            begin.event->action == action,
                        "the press must latch its coordinates and action");
                require(begin.maskActivationKey ==
                            (key == QStringLiteral("windows") || key == QStringLiteral("alt")),
                        "only Win and Alt need a menu mask");
                config.bindings.clear();
                config.captureAvailable = false;
                auto move = gesture.handle({InputKind::Move, {70, 80}}, config);
                require(
                    move.event && move.event->id == begin.event->id &&
                        move.event->kind == EventKind::Update && !move.consumed,
                    "modifier release and configuration changes must preserve the drag and cursor");
                auto finish =
                    gesture.handle({InputKind::Release, {72, 83}, binding->button}, config);
                require(finish.consumed && finish.event &&
                            finish.event->kind == EventKind::Finish &&
                            finish.event->id == begin.event->id && finish.event->action == action &&
                            finish.event->position == QPoint(72, 83),
                        "initiating release must carry the final position and original action");
                require(
                    !gesture.handle({InputKind::Release, {72, 83}, binding->button}, config).event,
                    "duplicate releases must never execute twice");
                auto cancel = gesture.handle({InputKind::Cancel}, config);
                require(cancel.event && cancel.event->id == begin.event->id,
                        "Escape must cancel a released gesture still awaiting capture readiness");
            }
        }
    }
    require(!globalMouseBinding(Action::ScreenshotCopy, {}), "unset combinations must not bind");
    require(!globalMouseBinding(Action::ScreenshotCopy, {{QStringLiteral("bad")}, buttons[0]}),
            "invalid modifiers must not bind");
    require(!globalMouseBinding(Action::ScreenshotCopy, {{keys[0]}, QStringLiteral("bad")}),
            "invalid buttons must not bind");
}

void multipleKeysMustBeHeldTogetherOnlyAtActivation() {
    const std::array keys{QStringLiteral("ctrl"), QStringLiteral("shift"), QStringLiteral("alt"),
                          QStringLiteral("windows")};
    const std::array modifiers{Qt::ControlModifier, Qt::ShiftModifier, Qt::AltModifier,
                               Qt::MetaModifier};
    for (int configuredMask = 1; configuredMask < 16; ++configuredMask) {
        QStringList selected;
        for (int bit = 0; bit < 4; ++bit) {
            if ((configuredMask & (1 << bit)) != 0) {
                selected.push_back(keys[bit]);
            }
        }
        const auto binding = globalMouseBinding(Action::ScreenshotTranslation,
                                                {selected, QStringLiteral("left_drag")});
        require(binding.has_value(), "every nonempty key subset must bind");
        const GlobalMouseConfiguration config{{*binding}, true};
        for (int heldMask = 0; heldMask < 16; ++heldMask) {
            GlobalMouseGesture gesture;
            Qt::KeyboardModifiers held;
            for (int bit = 0; bit < 4; ++bit) {
                if ((heldMask & (1 << bit)) != 0) {
                    held |= modifiers[bit];
                }
            }
            const auto result =
                gesture.handle({InputKind::Press, {20, 30}, Qt::LeftButton, held}, config);
            require(result.event.has_value() == (configuredMask == heldMask),
                    "only the complete simultaneous configured chord may activate");
            if (result.event) {
                const auto move = gesture.handle({InputKind::Move, {50, 70}}, {});
                const auto finish =
                    gesture.handle({InputKind::Release, {60, 80}, Qt::LeftButton}, {});
                require(
                    move.event && finish.event && finish.event->id == result.event->id &&
                        finish.event->action == Action::ScreenshotTranslation,
                    "activation must latch the action despite key release and settings changes");
            }
        }
    }
}

void directButtonDragPreservesQtInputAndLatchesAction() {
    GlobalMouseGesture gesture;
    const auto begin = gesture.beginButtonDrag(Action::ScreenshotOcr, {-20, 30});
    require(begin.event && !begin.consumed && !begin.maskActivationKey &&
                begin.event->action == Action::ScreenshotOcr &&
                begin.event->position == QPoint(-20, 30),
            "direct drags must start without activation keys and preserve the Qt press");
    require(!gesture.beginButtonDrag(Action::ScreenshotCopy, {}).event,
            "another action must not replace a pending drag");
    require(gesture.handle({InputKind::Move, {50, 80}}, {}).event.has_value(),
            "direct drag movement must survive disabled global capture settings");
    const auto finish = gesture.handle({InputKind::Release, {60, 90}, Qt::LeftButton}, {});
    require(finish.event && finish.event->kind == EventKind::Finish && !finish.consumed &&
                finish.event->id == begin.event->id &&
                finish.event->action == Action::ScreenshotOcr,
            "the original action must finish once and forward its balanced Qt release");
    require(!gesture.handle({InputKind::Release, {60, 90}, Qt::LeftButton}, {}).event,
            "duplicate local releases must not execute twice");
    gesture.cancel(begin.event->id);
    const auto next = gesture.beginButtonDrag(Action::ScreenshotFixed, {});
    require(next.event.has_value(), "a completed local drag must allow another gesture");
    gesture.cancel(next.event->id);
    require(!gesture.handle({InputKind::Release, {}, Qt::LeftButton}, {}).consumed,
            "cancelled local drags must still forward the Qt release");
}

void suppressionAndCancellationPreserveInputPairs() {
    auto config = configured();
    GlobalMouseGesture gesture;
    const GlobalMouseInput press{InputKind::Press, {10, 10}, Qt::LeftButton, Qt::ControlModifier};
    config.captureAvailable = false;
    require(!gesture.handle(press, config).consumed, "busy capture must leave desktop input alone");
    config.captureAvailable = true;
    config.bindings.push_back({Action::ScreenshotFixed, Qt::ControlModifier, Qt::LeftButton});
    require(!gesture.handle(press, config).event, "ambiguous persisted bindings must fail closed");
    config.bindings.removeLast();
    auto injected = press;
    injected.injected = true;
    require(!gesture.handle(injected, config).event, "injected input must not start a capture");
    auto chord = press;
    chord.heldButtons = Qt::RightButton;
    require(!gesture.handle(chord, config).consumed,
            "global mouse must not take over another application drag already in progress");
    const auto begin = gesture.handle(press, config);
    require(begin.event.has_value(), "valid gesture must start");
    require(gesture.handle({InputKind::Press, {}, Qt::BackButton}, config).consumed,
            "additional button presses must be swallowed during the drag");
    require(!gesture.handle({InputKind::Release, {}, Qt::BackButton}, config).event,
            "another button cannot finish the drag");
    gesture.cancel(begin.event->id + 1);
    require(gesture.active(), "stale cancellation must not touch the active gesture");
    gesture.cancel(begin.event->id);
    require(!gesture.active(), "cancellation must stop drag notifications");
    const auto release = gesture.handle({InputKind::Release, {}, Qt::LeftButton}, config);
    require(release.consumed && !release.event, "cancel must drain the swallowed initiating press");
    require(gesture.handle(press, config).event.has_value(),
            "a fresh gesture must work after cancellation");
}

void selectionRetainsThePhysicalAnchorUntilReady() {
    ScreenshotGlobalMouseDrag drag;
    drag.begin(42, {-500, -300});
    require(drag.update(42, {800, 600}) && !drag.ready(), "early moves must be buffered");
    require(drag.update(42, {750, 550}, true), "early release must be buffered");
    require(!drag.update(42, {900, 900}) && !drag.update(41, {0, 0}, true),
            "late moves and stale events must not change the release point");
    drag.setReady();
    require(drag.start() == QPoint(-500, -300) && drag.end() == QPoint(750, 550) && drag.released(),
            "capture readiness must preserve the original anchor and final release");
    const QRectF bounds(-600, -400, 2400, 1600);
    for (const QPointF end :
         {QPointF(-550, -350), QPointF(-450, -350), QPointF(-550, -250), QPointF(-450, -250)}) {
        const QRectF selection = ScreenshotGlobalMouseDrag::selection(drag.start(), end, bounds);
        require(selection.size() == QSizeF(50, 50),
                "all drag directions must normalize consistently");
    }
    require(ScreenshotGlobalMouseDrag::selection({0, 0}, {1, 1}, bounds).size() == QSizeF(1, 1),
            "one physical pixel in both dimensions is usable");
    require(ScreenshotGlobalMouseDrag::selection({0, 0}, {0, 30}, bounds).isEmpty() &&
                ScreenshotGlobalMouseDrag::selection({0, 0}, {30, 0}, bounds).isEmpty() &&
                ScreenshotGlobalMouseDrag::selection({0, 0}, {0, 0}, bounds).isEmpty(),
            "empty and single-axis gestures must not expand to a usable selection");
    require(ScreenshotGlobalMouseDrag::selection({-900, -900}, {9000, 9000}, bounds) == bounds,
            "selection endpoints must stay within the captured desktop");
    drag.reset();
    drag.setReady();
    require(!drag.active() && !drag.update(42, {0, 0}, true),
            "cancelled sessions cannot be revived");
}

void revealRefreshCatchesUpToLiveCursorOnlyWhileDragging() {
    ScreenshotGlobalMouseDrag drag;
    drag.begin(7, {100, 100});
    require(drag.update(7, {200, 200}) && !drag.ready(),
            "a pending drag must buffer paced movement before readiness");
    drag.setReady();
    drag.refreshEndFromLivePosition(QPoint(319, 407));
    require(drag.start() == QPoint(100, 100) && drag.end() == QPoint(319, 407) && !drag.released(),
            "activation must adopt the live cursor instead of the last paced event");
    drag.refreshEndFromLivePosition(std::nullopt);
    require(drag.end() == QPoint(319, 407),
            "an unavailable cursor position must keep the buffered end point");
    require(drag.update(7, {400, 400}, true), "a release after activation must buffer normally");
    drag.refreshEndFromLivePosition(QPoint(900, 900));
    require(drag.end() == QPoint(400, 400) && drag.released(),
            "a buffered release must never be overridden by the live cursor");
    ScreenshotGlobalMouseDrag inactive;
    inactive.refreshEndFromLivePosition(QPoint(5, 5));
    require(!inactive.active() && inactive.end() == QPoint(),
            "a reset drag must ignore live-cursor refreshes");
}

class FakeBackend final : public GlobalMouseBackend {
  public:
    void start(Handler callback, FailureHandler error) override {
        handler = std::move(callback);
        failure = std::move(error);
        ++starts;
    }
    void stop() override {
        ++stops;
    }
    void configure(const GlobalMouseConfiguration& value) override {
        configuration = value;
    }
    void cancel(quint64 id) override {
        cancelled = id;
    }
    void beginButtonDrag(Action action) override {
        buttonAction = action;
        ++buttonDrags;
    }
    Action buttonAction = Action::ScreenshotCopy;
    int buttonDrags = 0;
    Handler handler;
    FailureHandler failure;
    GlobalMouseConfiguration configuration;
    quint64 cancelled = 0;
    int starts = 0;
    int stops = 0;
};

class FakeHotkeyBackend final : public GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    GlobalShortcutValidationResult validateShortcut(const QString& shortcut) const override {
        return {shortcut, true, GlobalShortcutFailureReason::None};
    }
    GlobalShortcutBackendResult registerShortcut(int id, const QString&) override {
        registrationId = id;
        return {true};
    }
    void unregisterShortcut(int) override {}

    ActivationHandler handler;
    int registrationId = 0;
};

void hotkeySuppressionNeverDisablesMouseGestures() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "test storage directory must be available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(
        storage.initialize({temporary.filePath(QStringLiteral("bin")), temporary.path()}));
    require(storage.isInitialized(), "test storage must initialize");
    auto& store = storage.configuration();
    const QString fullscreenKey =
        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
    require(store.setValue(fullscreenKey, true), "initial fullscreen suppression must persist");
    {
        bool fullscreen = true;
        auto hotkeyBackend = std::make_unique<FakeHotkeyBackend>();
        auto* hotkeyInput = hotkeyBackend.get();
        GlobalShortcutManager hotkeys(std::move(hotkeyBackend), nullptr,
                                      [&fullscreen]() { return fullscreen; });
        hotkeys.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+F9")});
        int hotkeyActivations = 0;
        QObject::connect(&hotkeys, &GlobalShortcutManager::activated, &hotkeys,
                         [&hotkeyActivations](GlobalShortcutAction) { ++hotkeyActivations; });
        auto mouseBackend = std::make_unique<FakeBackend>();
        auto* mouseInput = mouseBackend.get();
        GlobalMouseManager mouse(std::move(mouseBackend));
        mouse.setCaptureAvailable(true);
        mouse.initialize();
        const auto requireMouseActivation = [&]() {
            GlobalMouseGesture gesture;
            const GlobalMouseInput press{
                InputKind::Press, {10, 20}, Qt::LeftButton, Qt::MetaModifier};
            const auto result = gesture.handle(press, mouseInput->configuration);
            require(result.consumed && result.event &&
                        result.event->action == Action::ScreenshotCopy,
                    "hotkey suppression must leave configured mouse gestures active");
        };
        requireMouseActivation();
        for (const bool enabled : {false, true}) {
            hotkeys.setGlobalHotkeysEnabled(enabled);
            for (const bool suppressFullscreen : {false, true}) {
                require(store.setValue(fullscreenKey, suppressFullscreen),
                        "fullscreen suppression changes must persist");
                for (const bool focusedFullscreen : {false, true}) {
                    fullscreen = focusedFullscreen;
                    const int previousActivations = hotkeyActivations;
                    hotkeyInput->handler(hotkeyInput->registrationId);
                    require(hotkeyActivations - previousActivations ==
                                static_cast<int>(enabled && !(suppressFullscreen && fullscreen)),
                            "global hotkeys must obey manual and fullscreen suppression");
                    requireMouseActivation();
                }
            }
        }
        mouse.setCaptureAvailable(false);
        GlobalMouseGesture gesture;
        require(!gesture
                     .handle({InputKind::Press, {}, Qt::LeftButton, Qt::MetaModifier},
                             mouseInput->configuration)
                     .event,
                "busy capture must still block mouse activation");
    }
    storage.shutdown();
}

void managerLoadsLiveSettingsAndCoalescesOnlyMovement() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "test storage directory must be available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(
        storage.initialize({temporary.filePath(QStringLiteral("bin")), temporary.path()}));
    require(storage.isInitialized(), "test storage must initialize");
    auto backend = std::make_unique<FakeBackend>();
    auto* input = backend.get();
    GlobalMouseManager manager(std::move(backend));
    manager.setCaptureAvailable(true);
    manager.initialize();
    manager.initialize();
    require(input->starts == 1 && input->configuration.bindings.size() == 3,
            "initialization must be idempotent and load three default bindings");
    const std::array defaultActions{Action::ScreenshotCopy, Action::ScreenshotFixed,
                                    Action::ScreenshotOcr};
    const std::array defaultButtons{Qt::LeftButton, Qt::MiddleButton, Qt::RightButton};
    for (qsizetype index = 0; index < 3; ++index) {
        const auto& binding = input->configuration.bindings[index];
        require(binding.action == defaultActions[index] && binding.modifiers == Qt::MetaModifier &&
                    binding.button == defaultButtons[index],
                "default bindings must reach the backend as Windows plus left, middle, and right");
    }
    for (const auto* key : {"global_mouse/screenshot_copy", "global_mouse/screenshot_fixed",
                            "global_mouse/screenshot_ocr"}) {
        require(storage.configuration().setValue(QString::fromLatin1(key), QJsonObject{}),
                "explicitly clearing a default must be accepted");
    }
    manager.beginButtonDrag(Action::ScreenshotOcr);
    require(input->buttonDrags == 1 && input->buttonAction == Action::ScreenshotOcr,
            "direct button drags must work with unset bindings");
    manager.setCaptureAvailable(false);
    manager.beginButtonDrag(Action::ScreenshotFixed);
    require(input->buttonDrags == 1, "busy capture must reject a direct button drag");
    manager.setCaptureAvailable(true);
    auto& store = storage.configuration();
    require(
        store.setValue(QStringLiteral("global_mouse/screenshot_quick_save"),
                       QJsonObject{{QStringLiteral("activation_key"), QStringLiteral("shift")},
                                   {QStringLiteral("mouse_button"), QStringLiteral("wheel_drag")}}),
        "test combination must persist");
    require(input->configuration.bindings.size() == 1 &&
                input->configuration.bindings[0].action == Action::ScreenshotQuickSave,
            "committed settings must update the backend immediately");
    require(
        store.setValue(QStringLiteral("global_mouse/screenshot_quick_save"),
                       QJsonObject{{QStringLiteral("activation_key"),
                                    QJsonArray{QStringLiteral("ctrl"), QStringLiteral("shift")}},
                                   {QStringLiteral("mouse_button"), QStringLiteral("wheel_drag")}}),
        "multiple activation keys must persist");
    require(input->configuration.bindings[0].modifiers == (Qt::ControlModifier | Qt::ShiftModifier),
            "all persisted activation keys must reach the native backend");
    require(store.setValue(
                QStringLiteral("global_mouse/screen_recording"),
                QJsonObject{{QStringLiteral("activation_key"), QJsonArray{QStringLiteral("alt")}},
                            {QStringLiteral("mouse_button"), QStringLiteral("left_drag")}}),
            "screen recording combination must persist");
    require(input->configuration.bindings.size() == 2 &&
                input->configuration.bindings.last().action == Action::ScreenRecording &&
                input->configuration.bindings.last().modifiers == Qt::AltModifier &&
                input->configuration.bindings.last().button == Qt::LeftButton,
            "screen recording settings must reach the native backend immediately");
    manager.setCaptureAvailable(false);
    require(!input->configuration.captureAvailable,
            "capture availability must reach the native backend");
    QVector<GlobalMouseDragEvent> delivered;
    QObject::connect(&manager, &GlobalMouseManager::dragEvent, &manager,
                     [&delivered](const auto& event) { delivered.push_back(event); });
    int failures = 0;
    QObject::connect(&manager, &GlobalMouseManager::operationFailed, &manager,
                     [&failures](const QString&) { ++failures; });
    input->handler({EventKind::Begin, 7, Action::ScreenshotQuickSave, {10, 20}});
    for (int i = 0; i < 1000; ++i) {
        input->handler({EventKind::Update, 7, Action::ScreenshotQuickSave, {i, 25}});
    }
    input->handler({EventKind::Finish, 7, Action::ScreenshotQuickSave, {1100, 30}});
    input->failure(123);
    QCoreApplication::processEvents();
    require(delivered.size() == 2 && delivered[0].position == QPoint(10, 20) &&
                delivered[1].kind == EventKind::Finish && delivered[1].position == QPoint(1100, 30),
            "the exact release must supersede movement not yet presented");
    require(failures == 1, "backend failure must notify the application");
    manager.cancelGesture(7);
    require(input->cancelled == 7, "capture cancellation must reach native gesture ownership");
    require(store.setValue(QStringLiteral("global_mouse/screenshot_quick_save"), QJsonObject{}),
            "reset must persist");
    require(store.setValue(QStringLiteral("global_mouse/screen_recording"), QJsonObject{}),
            "screen recording reset must persist");
    require(input->configuration.bindings.isEmpty(), "reset must remove runtime bindings");
    input->handler({EventKind::Begin, 8, Action::ScreenshotCopy, {}});
    manager.shutdown();
    QCoreApplication::processEvents();
    require(input->stops == 1 && delivered.size() == 2, "shutdown must discard queued input");
    storage.shutdown();
}

void managerPacesSustainedMovementAndDeliversTerminalEventsImmediately() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(
        storage.initialize({temporary.filePath(QStringLiteral("bin")), temporary.path()}));
    require(storage.isInitialized(), "paced delivery test storage must initialize");
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* input = backend.get();
        GlobalMouseManager manager(std::move(backend));
        manager.initialize();
        auto* timer = manager.findChild<QTimer*>(QStringLiteral("globalMouseFrameTimer"));
        require(timer != nullptr && timer->isSingleShot() && timer->timerType() == Qt::PreciseTimer,
                "movement pacing must use a single-shot precise frame timer");
        QVector<GlobalMouseDragEvent> delivered;
        QObject::connect(&manager, &GlobalMouseManager::dragEvent, &manager,
                         [&delivered](const auto& event) { delivered.push_back(event); });
        const auto drain = [&] { QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall); };
        const auto deadline = [&] {
            require(timer->isActive(), "a pending frame must have a deadline");
            timer->stop();
            require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
                    "the deterministic frame deadline must be delivered");
        };
        input->handler({EventKind::Begin, 1, Action::ScreenshotCopy, {10, 20}});
        drain();
        require(delivered.size() == 1 && timer->isActive() && timer->interval() > 0,
                "Begin must arrive immediately and establish presentation pacing");
        for (int frame = 0; frame < 3; ++frame) {
            for (int i = 0; i < 1000; ++i) {
                input->handler({EventKind::Update, 1, Action::ScreenshotCopy, {i, frame}});
                drain();
            }
            require(delivered.size() == frame + 1,
                    "interleaved GUI delivery must not let movement bypass the frame deadline");
            deadline();
            require(delivered.size() == frame + 2 &&
                        delivered.back().position == QPoint(999, frame),
                    "each deadline must present exactly the newest position");
        }
        input->handler({EventKind::Update, 1, Action::ScreenshotCopy, {1001, 4}});
        input->handler({EventKind::Finish, 1, Action::ScreenshotCopy, {1002, 5}});
        drain();
        require(delivered.size() == 5 && delivered.back().kind == EventKind::Finish &&
                    delivered.back().position == QPoint(1002, 5) && !timer->isActive(),
                "Finish must bypass pacing, supersede pending movement, and stop its timer");
        QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        require(delivered.size() == 5, "a stale deadline must not deliver movement after Finish");
        input->handler({EventKind::Cancel, 1, Action::ScreenshotCopy, {}});
        drain();
        require(delivered.size() == 6 && delivered.back().kind == EventKind::Cancel &&
                    !timer->isActive(),
                "a released capture must remain cancellable while preparation is pending");
        manager.cancelGesture(1);
        input->handler({EventKind::Cancel, 1, Action::ScreenshotCopy, {}});
        drain();
        require(delivered.size() == 6, "acknowledged cancellation must reject stale events");
        delivered.removeLast();
        input->handler({EventKind::Begin, 2, Action::ScreenshotCopy, {}});
        drain();
        input->handler({EventKind::Update, 2, Action::ScreenshotCopy, {5, 5}});
        input->handler({EventKind::Cancel, 2, Action::ScreenshotCopy, {}});
        drain();
        require(delivered.size() == 7 && delivered.back().kind == EventKind::Cancel &&
                    !timer->isActive(),
                "Cancel must bypass the deadline and suppress its pending movement");
        input->handler({EventKind::Begin, 3, Action::ScreenshotCopy, {}});
        drain();
        deadline();
        require(!timer->isActive(), "a stationary pointer must not keep waking the GUI");
        input->handler({EventKind::Update, 3, Action::ScreenshotCopy, {6, 6}});
        drain();
        require(delivered.back().position == QPoint(6, 6) && timer->isActive(),
                "movement after an idle deadline must resume without an extra frame of delay");
        input->handler({EventKind::Update, 3, Action::ScreenshotCopy, {7, 7}});
        manager.cancelGesture(3);
        drain();
        require(delivered.back().position == QPoint(6, 6) && !timer->isActive(),
                "controller cancellation must discard paced movement");

        const auto oldHandler = input->handler;
        const auto oldFailure = input->failure;
        int failures = 0;
        QObject::connect(&manager, &GlobalMouseManager::operationFailed, &manager,
                         [&failures](const QString&) { ++failures; });
        oldHandler({EventKind::Begin, 4, Action::ScreenshotCopy, {}});
        oldFailure(1);
        manager.shutdown();
        manager.initialize();
        const auto count = delivered.size();
        oldHandler({EventKind::Update, 4, Action::ScreenshotCopy, {9, 9}});
        input->handler({EventKind::Begin, 5, Action::ScreenshotCopy, {11, 11}});
        drain();
        require(delivered.size() == count + 1 && delivered.back().id == 5 && failures == 0,
                "restart must reject stale input and failure callbacks without losing new input");

        QObject::connect(&manager, &GlobalMouseManager::dragEvent, &manager,
                         [&](const auto& event) {
                             if (event.id == 6 && event.kind == EventKind::Begin) {
                                 manager.shutdown();
                                 manager.initialize();
                             }
                         });
        input->handler({EventKind::Begin, 6, Action::ScreenshotCopy, {}});
        input->handler({EventKind::Finish, 6, Action::ScreenshotCopy, {20, 20}});
        drain();
        require(delivered.back().id == 6 && delivered.back().kind == EventKind::Begin &&
                    !timer->isActive(),
                "reentrant restart must stop delivery of the old batch and its timer");

        input->handler({EventKind::Begin, 7, Action::ScreenshotCopy, {}});
        drain();
        input->handler({EventKind::Finish, 7, Action::ScreenshotCopy, {1, 1}});
        input->handler({EventKind::Begin, 8, Action::ScreenshotCopy, {2, 2}});
        deadline();
        const auto beforeStaleWake = delivered.size();
        input->handler({EventKind::Update, 8, Action::ScreenshotCopy, {3, 3}});
        drain();
        require(delivered.size() == beforeStaleWake,
                "a queued wake overtaken by the timer must not present new movement early");
        deadline();
        require(delivered.size() == beforeStaleWake + 1 &&
                    delivered.back().position == QPoint(3, 3),
                "movement deferred by a stale wake must arrive at the next deadline");
        input->handler({EventKind::Finish, 8, Action::ScreenshotCopy, {4, 4}});
        drain();
        const auto completedCount = delivered.size();
        input->handler({EventKind::Update, 8, Action::ScreenshotCopy, {5, 5}});
        drain();
        require(delivered.size() == completedCount && !timer->isActive(),
                "released captures must reject movement while remaining cancellable");
        input->handler({EventKind::Cancel, 8, Action::ScreenshotCopy, {}});
        manager.cancelGesture(8);
        drain();
        require(delivered.size() == completedCount && !timer->isActive(),
                "completion acknowledgement must discard queued cancellation after release");

        int frameBudget = 0;
        const auto slowSubscriber = QObject::connect(
            &manager, &GlobalMouseManager::dragEvent, &manager, [&](const auto& event) {
                if (event.id == 9) {
                    if (event.kind == EventKind::Begin) {
                        frameBudget = timer->interval();
                    }
                    // Force an overrun without depending on machine speed or timer delivery.
                    QThread::msleep(static_cast<unsigned long>(frameBudget + 20));
                }
            });
        input->handler({EventKind::Begin, 9, Action::ScreenshotCopy, {}});
        drain();
        require(timer->isActive() && timer->interval() == 1,
                "a subscriber exceeding the frame budget must not incur another full interval");
        input->handler({EventKind::Update, 9, Action::ScreenshotCopy, {10, 10}});
        deadline();
        require(timer->isActive() && timer->interval() == 1 &&
                    delivered.back().position == QPoint(10, 10),
                "update subscribers must consume the frame budget just like capture startup");
        QObject::disconnect(slowSubscriber);
        manager.cancelGesture(9);
    }
    storage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--native-hook-smoke"))) {
        require(QGuiApplication::platformName() == QStringLiteral("windows"),
                "native smoke test requires the Windows platform");
        auto backend = createGlobalMouseBackend();
        backend->configure({});
        std::atomic_int failures = 0;
        for (int iteration = 0; iteration < 2; ++iteration) {
            backend->start([](const GlobalMouseDragEvent&) {},
                           [&failures](quint32) { ++failures; });
            backend->stop();
        }
        require(failures == 0, "Windows hooks must install and shut down cleanly");
        return 0;
    }
    everyCombinationMatchesExactlyAndLatchesItsAction();
#ifdef Q_OS_WIN
    globalMouseNativeActivationKeyTests();
    globalMouseNativePerformanceTests();
    globalMouseNativeStopFromWorkerThreadTests();
#endif
    multipleKeysMustBeHeldTogetherOnlyAtActivation();
    directButtonDragPreservesQtInputAndLatchesAction();
    suppressionAndCancellationPreserveInputPairs();
    selectionRetainsThePhysicalAnchorUntilReady();
    revealRefreshCatchesUpToLiveCursorOnlyWhileDragging();
    hotkeySuppressionNeverDisablesMouseGestures();
    managerLoadsLiveSettingsAndCoalescesOnlyMovement();
    managerPacesSustainedMovementAndDeliversTerminalEventsImmediately();
    return 0;
}
