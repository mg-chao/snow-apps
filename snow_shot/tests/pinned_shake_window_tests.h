#pragma once

namespace pinned_shake_tests {
using Access = ScreenshotPinnedWindowTestAccess;
using Result = ScreenshotPinnedWindow::HideOtherWindowsResult;
using Settings = snow_shot::storage::PinToScreenSettings;

ScreenshotPinnedWindow::Config config() {
    auto value = cachedOcrPinConfig(nullptr);
    value.recognitionResults = {};
    value.enableEditing = false;
    value.persistenceWriter = [](const auto&) {};
    return value;
}

void present(ScreenshotPinnedWindow& window, const ScreenshotPinnedWindow::Config& value) {
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    require(window.present(value), "present shake fixture");
    translation_tests::waitUntil(
        [&] { return window.sourcePinAvailable() && Access::shakeContentReady(window); },
        "shake fixture must publish its first frame");
    translation_tests::flushEvents();
}

void shake(ScreenshotPinnedWindow& window, const QPointF& origin = {}, int firstTime = 0) {
    for (int leg = 1; leg <= 6; ++leg)
        Access::sampleShake(window, origin + QPointF((leg % 2) ? 64 : 0, 0), firstTime + leg * 80);
}

void ordinaryDragCorrectionsKeepPeersVisible() {
    IsolatedPinnedStorage isolated;
    ScreenshotPinnedWindow window;
    ScreenshotPinnedWindow peer;
    int requests = 0;
    auto value = config();
    value.shakeHideOtherWindowsRequested = [&](const QString&) {
        ++requests;
        const bool hidden = peer.requestHide();
        return Result{hidden, hidden};
    };
    present(peer, config());
    present(window, value);
    struct Motion {
        qreal amplitude;
        int legs;
        int interval;
    };
    for (const auto motion :
         {Motion{28, 12, 40}, Motion{40, 12, 40}, Motion{96, 4, 100}, Motion{64, 6, 300}}) {
        require(Access::beginShakeDrag(window), "begin ordinary adjustment drag");
        for (int leg = 1; leg <= motion.legs; ++leg)
            Access::sampleShake(window, {(leg % 2) ? motion.amplitude : 0, 0},
                                leg * motion.interval);
        require(!Access::shakePending(window), "ordinary adjustments must not queue a hide");
        Access::finishShakeDrag(window);
        QCoreApplication::processEvents();
        require(requests == 0 && peer.isVisible() && !Access::shakeMessage(window),
                "small, brief or slow corrections must leave other pinned windows visible");
    }
    window.close();
    peer.close();
}

void callbackAndMessagePreserveDrag() {
    IsolatedPinnedStorage isolated;
    require(Settings().shakeWindowToHideOthers(), "shake setting defaults on");
    ScreenshotPinnedWindow window;
    auto value = config();
    int requests = 0;
    value.shakeHideOtherWindowsRequested = [&](const QString& id) {
        require(id == window.persistenceId(), "shake must exclude the grabbed pin by identity");
        ++requests;
        return Result{true, true};
    };
    present(window, value);
    require(Access::beginShakeDrag(window), "begin controlled shake drag");
    const auto grabber = QWidget::mouseGrabber();
    const QRect before = window.currentNativeGeometry();
    shake(window);
    require(requests == 0 && Access::shakePending(window), "hide must be queued outside sampling");
    QCoreApplication::processEvents();
    require(requests == 1 && window.isVisible() && Access::shakeDragActive(window) &&
                QWidget::mouseGrabber() == grabber && window.currentNativeGeometry() == before,
            "hiding peers must preserve the grabbed window and drag capture");
    auto* message = Access::shakeMessage(window);
    require(message && message->isOpen() &&
                message->content() == QStringLiteral("Other windows hidden.") &&
                adqt::widgets::AdMessageService::instance(&window)->ownerWindow() == &window &&
                Access::shakeMessageRemaining(window) > 0 &&
                Access::shakeMessageRemaining(window) <= 3000,
            "shake success must have a transient message owned by the grabbed window");
    shake(window, {}, 350);
    QCoreApplication::processEvents();
    require(requests == 1, "one drag must trigger at most once");
    Access::moveShakeDrag(window, QPointF(70, 10));
    require(window.currentNativeGeometry() != before && Access::shakeDragActive(window),
            "the grabbed window must continue moving after the hide operation");
    Access::finishShakeDrag(window);

    class Translator final : public QTranslator {
      public:
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (qstrcmp(context, "ScreenshotPinnedWindow") == 0 &&
                qstrcmp(source, "Other windows hidden.") == 0)
                return QStringLiteral("Translated shake message");
            return {};
        }
    } translator;
    const qint64 remaining = Access::shakeMessageRemaining(window);
    QCoreApplication::installTranslator(&translator);
    QEvent language(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&window, &language);
    require(message == Access::shakeMessage(window) &&
                message->content() == QStringLiteral("Translated shake message") &&
                Access::shakeMessageRemaining(window) <= remaining,
            "language changes must update the keyed message without extending its lifetime");
    QCoreApplication::removeTranslator(&translator);
    window.close();
}

void pendingActionsRespectCancellationAndSettings() {
    IsolatedPinnedStorage isolated;
    for (const QString& reason :
         {QStringLiteral("release"), QStringLiteral("cancel"), QStringLiteral("disable"),
          QStringLiteral("disable-enable"), QStringLiteral("hide"), QStringLiteral("reuse"),
          QStringLiteral("group")}) {
        require(Settings().setShakeWindowToHideOthers(true),
                "enable shake for pending action test");
        ScreenshotPinnedWindow window;
        auto value = config();
        int requests = 0;
        value.shakeHideOtherWindowsRequested = [&](const QString&) {
            ++requests;
            return Result{true, false};
        };
        present(window, value);
        require(Access::beginShakeDrag(window), "begin pending shake drag");
        shake(window);
        if (reason == u"release")
            Access::finishShakeDrag(window);
        else if (reason == u"cancel")
            Access::finishShakeDrag(window, true);
        else if (reason == u"disable" || reason == u"disable-enable") {
            require(Settings().setShakeWindowToHideOthers(false), "disable a recognized shake");
            if (reason == u"disable-enable")
                require(Settings().setShakeWindowToHideOthers(true), "reenable before execution");
        } else if (reason == u"hide")
            window.hide();
        else if (reason == u"reuse") {
            Access::finishShakeDrag(window);
            ScreenshotClipboardContent replacement;
            replacement.image = value.imageSource.materializedImage;
            require(window.automationReplaceContent(std::move(replacement)),
                    "replace content before the old shake callback executes");
        } else
            window.setGroupId(QStringLiteral("changed"));
        QCoreApplication::processEvents();
        require(requests == (reason == u"release" ? 1 : 0),
                "only normal release may preserve a queued shake action");
        require(!Access::shakeMessage(window),
                "empty or canceled operations must not show success");
        Access::finishShakeDrag(window, true);
        window.close();
    }
}

void liveSettingAndResetsRequireAFreshDrag() {
    IsolatedPinnedStorage isolated;
    ScreenshotPinnedWindow window;
    auto value = config();
    int requests = 0;
    value.shakeHideOtherWindowsRequested = [&](const QString&) {
        ++requests;
        return Result{true, true};
    };
    present(window, value);
    require(Settings().setShakeWindowToHideOthers(false), "disable existing windows");
    require(Access::beginShakeDrag(window) && !Access::shakeArmed(window),
            "disabled drag is unarmed");
    require(Settings().setShakeWindowToHideOthers(true), "enable during the drag");
    shake(window);
    QCoreApplication::processEvents();
    require(requests == 0, "enabling during a drag must require a fresh press");
    Access::finishShakeDrag(window);
    require(Access::beginShakeDrag(window), "begin fresh enabled drag");
    Access::sampleShake(window, {64, 0}, 80);
    Access::sampleShake(window, {0, 0}, 160);
    require(Settings().setShakeWindowToHideOthers(false) &&
                Settings().setShakeWindowToHideOthers(true),
            "toggle setting during incomplete recognition");
    shake(window, {}, 200);
    QCoreApplication::processEvents();
    require(requests == 0, "setting changes must discard incomplete recognition");
    Access::finishShakeDrag(window);
    require(Access::beginShakeDrag(window), "begin drag after a live toggle");
    shake(window);
    Access::finishShakeDrag(window);
    QCoreApplication::processEvents();
    require(requests == 1, "fresh drag must honor the newly enabled setting");
    window.close();
}

void failuresAndEmptyTargetsDoNotShowSuccess() {
    IsolatedPinnedStorage isolated;
    for (const auto result : {Result{false, false}, Result{false, true}, Result{true, false}}) {
        ScreenshotPinnedWindow window;
        auto value = config();
        value.shakeHideOtherWindowsRequested = [result](const QString&) { return result; };
        present(window, value);
        require(Access::beginShakeDrag(window), "begin unsuccessful shake drag");
        shake(window);
        QCoreApplication::processEvents();
        require(!Access::shakeMessage(window), "only successful visibility changes show success");
        Access::finishShakeDrag(window);
        window.close();
    }
    ScreenshotPinnedWindow solo;
    present(solo, config());
    require(Access::beginShakeDrag(solo), "begin standalone drag without peers");
    shake(solo);
    QCoreApplication::processEvents();
    require(!Access::shakeMessage(solo) && solo.isVisible(), "standalone no-op remains quiet");
    Access::finishShakeDrag(solo);
    solo.close();
}

void fallbackHidesOnlyPeersAndPreservesSnapshots() {
    IsolatedPinnedStorage isolated;
    ScreenshotPinnedWindow source;
    ScreenshotPinnedWindow peer;
    ScreenshotPinnedWindow differentGroup;
    present(source, config());
    auto peerConfig = config();
    std::optional<snow_shot::storage::PinnedWindowRecord> hidden;
    peerConfig.persistenceHider = [&](const auto& record) {
        hidden = record;
        return true;
    };
    present(peer, peerConfig);
    auto other = config();
    other.groupId = QStringLiteral("another-group");
    present(differentGroup, other);
    const auto before = peer.persistenceSnapshot();
    require(Access::beginShakeDrag(source), "begin fallback visibility drag");
    shake(source);
    QCoreApplication::processEvents();
    require(source.isVisible() && !peer.isVisible() && differentGroup.isVisible() && hidden &&
                hidden->hidden && !hidden->ignored && hidden->image == before.image &&
                hidden->nativeGeometry == before.nativeGeometry &&
                hidden->canvasSession == before.canvasSession,
            "fallback must hide only same-group peers and preserve their document and placement");
    Access::finishShakeDrag(source);
    source.close();
    differentGroup.close();
}

void dpiChangesAndSharedGeometryCannotCompleteOldShakes() {
    IsolatedPinnedStorage isolated;
    ScreenshotPinnedWindow window;
    auto value = config();
    int requests = 0;
    value.shakeHideOtherWindowsRequested = [&](const QString&) {
        ++requests;
        return Result{true, false};
    };
    present(window, value);
    require(Access::beginShakeDrag(window), "begin display transition fixture");
    for (int leg = 1; leg <= 5; ++leg)
        Access::sampleShake(window, {(leg % 2) ? 64. : 0, 0}, leg * 80);
    Access::changeShakeScale(window);
    Access::sampleShake(window, {800, 0}, 440);
    for (int leg = 1; leg <= 5; ++leg)
        Access::sampleShake(window, {(leg % 2) ? 736. : 800., 0}, 440 + leg * 80);
    require(!Access::shakePending(window), "DPI transition must discard the old sequence");
    Access::sampleShake(window, {800, 0}, 920);
    QCoreApplication::processEvents();
    require(requests == 1, "a new complete shake after a DPI transition must still work");
    Access::finishShakeDrag(window);
    window.close();

    snow_shot::presentation::PinnedWindowSelectionController selection;
    ScreenshotPinnedWindow first;
    ScreenshotPinnedWindow second;
    value.selectionController = &selection;
    present(first, value);
    present(second, value);
    selection.toggleSelection(&first);
    selection.toggleSelection(&second);
    require(selection.selectedCount() == 2 && selection.beginGeometry(&first, {}),
            "begin selected-window group move");
    for (int leg = 1; leg <= 6; ++leg)
        selection.updateGeometry(QPointF((leg % 2) ? 64 : 0, 0));
    QCoreApplication::processEvents();
    require(requests == 1 && !Access::shakeArmed(first) && !Access::shakeArmed(second) &&
                first.isVisible() && second.isVisible(),
            "moving a shared selection must not arm shake recognition");
    selection.endGeometry(false);
    first.close();
    second.close();
}

void peerContentAndWindowModesSurviveHiding() {
    IsolatedPinnedStorage isolated;
    for (const QString& kind :
         {QStringLiteral("image"), QStringLiteral("text"), QStringLiteral("animation"),
          QStringLiteral("thumbnail"), QStringLiteral("hide-to-top")}) {
        ScreenshotPinnedWindow source;
        ScreenshotPinnedWindow peer;
        auto value = config();
        if (kind == u"text") {
            value.formattedTextDocument = std::make_shared<QTextDocument>();
            value.formattedTextDocument->setPlainText(QStringLiteral("Preserved text pin"));
            value.formattedPlainText = QStringLiteral("Preserved text pin");
            value.originalClipboardContent.text = value.formattedPlainText;
        } else if (kind == u"animation") {
            const auto animation = snow_shot::image_codec::decodeAnimationSource(
                pinned_animation_fixtures::images().front().bytes);
            require(animation && animation->isAnimated(), "decode animated peer fixture");
            value.animationSource = animation;
            value.canvasSourceRect = QRectF(QPointF(), animation->canvasSize);
            value.imageSource =
                ScreenshotImageSource::fromImage(animation->firstFrame, value.canvasSourceRect);
            value.contentCanvasRect = value.surfaceCanvasRect = value.canvasSourceRect;
            value.initialWindowSize = animation->canvasSize;
            value.nativeGeometry =
                physicalPinGeometry(*value.screen, {80, 80}, animation->canvasSize);
        }
        std::optional<snow_shot::storage::PinnedWindowRecord> hidden;
        value.persistenceHider = [&](const auto& record) {
            hidden = record;
            return true;
        };
        present(peer, value);
        if (kind == u"thumbnail")
            Access::thumbnailForHideTest(peer, true);
        else if (kind == u"hide-to-top") {
            auto& controller = Access::hideToTop(peer);
            require(controller.enter(screenshot_pinned_hide_to_top::screenGeometry(peer.screen())),
                    "enter hide-to-top before hiding a peer");
            controller.animation().setCurrentTime(controller.animation().duration());
        }
        const auto before = peer.persistenceSnapshot();
        present(source, config());
        require(Access::beginShakeDrag(source), "begin mode preservation shake");
        shake(source);
        QCoreApplication::processEvents();
        require(!peer.isVisible() && hidden && hidden->hidden && !hidden->ignored &&
                    hidden->nativeGeometry == before.nativeGeometry &&
                    hidden->placement == before.placement && hidden->image == before.image &&
                    hidden->originalText == before.originalText &&
                    hidden->animationSource == before.animationSource &&
                    hidden->thumbnailMode == before.thumbnailMode &&
                    hidden->hideToTopMode == before.hideToTopMode,
                "shaking must preserve every peer's content, placement and presentation mode");
        Access::finishShakeDrag(source);
        source.close();
    }
}

void serviceHidesPersistedAndPendingPeers() {
    IsolatedPinnedStorage isolated;
    using namespace snow_shot;
    auto& storage = storage::ApplicationStorage::instance();
    auto& repository = storage.pinnedWindows();
    require(Settings().setAutomaticTextRecognition(false), "disable recognition for service test");
    presentation::PinnedWindowGroupManager groups(&repository);
    ScreenshotSelectionExportUiServices service(nullptr, nullptr, nullptr, {}, {}, &groups);
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen, "service shake fixture requires a screen");
    QImage image(100, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::green);
    const QRect geometry = physicalPinGeometry(*screen, {60, 60}, image.size());
    auto add = [&] {
        require(service.presentPinnedImage(image, screen, geometry, image.size()),
                "create service pin");
        translation_tests::flushEvents();
    };
    add();
    add();
    translation_tests::waitUntil([&] { return groups.liveWindows().size() == 2; },
                                 "two service pins must become live");
    auto windows = groups.liveWindows();
    QPointer<ScreenshotPinnedWindow> source = windows[0];
    QPointer<ScreenshotPinnedWindow> peer = windows[1];
    translation_tests::waitUntil(
        [&] {
            const auto sourceRecord = repository.summary(source->persistenceId());
            const auto peerRecord = repository.summary(peer->persistenceId());
            return Access::shakeContentReady(*source) && Access::shakeContentReady(*peer) &&
                   sourceRecord && !sourceRecord->pending && peerRecord && !peerRecord->pending;
        },
        "the first two service peers must publish and persist before adding a loading peer");
    const QString peerId = peer->persistenceId();
    const auto before = peer->persistenceSnapshot();

    ScreenshotImageLoadCallback delayed;
    require(service.presentPinnedImage({}, screen, geometry, image.size(), {}, {}, 1, {},
                                       [&](QObject*, ScreenshotImageLoadCallback completion) {
                                           delayed = std::move(completion);
                                       }),
            "create a loading peer before the shake");
    translation_tests::waitUntil([&] { return bool(delayed); }, "pending loader must start");
    // Deliver the loading shell's queued show/activation before injecting the next pointer press.
    translation_tests::flushEvents();
    source->activateWindow();
    translation_tests::waitUntil([&] { return QApplication::activeWindow() == source; },
                                 "the grabbed pin must own activation before its pointer press");
    class InputTrace final : public QObject {
      public:
        ScreenshotPinnedWindow* owner = nullptr;
        QStringList events;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (watched == owner &&
                (event->type() == QEvent::WindowDeactivate ||
                 event->type() == QEvent::UngrabMouse || event->type() == QEvent::MouseMove ||
                 event->type() == QEvent::Hide))
                events.append(QStringLiteral("event=%1 buttons=%2 activeOwner=%3 %4")
                                  .arg(int(event->type()))
                                  .arg(int(QGuiApplication::mouseButtons()))
                                  .arg(QApplication::activeWindow() == owner)
                                  .arg(Access::shakeDiagnostics(*owner)));
            return false;
        }
    } inputTrace;
    inputTrace.owner = source;
    qApp->installEventFilter(&inputTrace);
    const auto removeInputTrace = qScopeGuard([&] { qApp->removeEventFilter(&inputTrace); });
    require(Access::beginShakeDrag(*source), "begin service-backed shake drag");
    shake(*source);
    QCoreApplication::processEvents();
    if (!Access::shakeMessage(*source) || !Access::shakeDragActive(*source)) {
        const auto summary = repository.summary(peerId);
        std::cerr << "Service shake: owner=" << source->isVisible()
                  << " peer=" << (peer && peer->isVisible())
                  << " hidden=" << (summary && summary->hidden)
                  << " ignored=" << (summary && summary->ignored) << " "
                  << Access::shakeDiagnostics(*source).toStdString() << '\n';
        for (const auto& event : inputTrace.events)
            std::cerr << event.toStdString() << '\n';
    }
    require(source && source->isVisible() && (!peer || !peer->isVisible()) &&
                repository.summary(peerId)->hidden && !repository.summary(peerId)->ignored &&
                Access::shakeDragActive(*source) && Access::shakeMessage(*source),
            "service-backed shake must persist hidden state and preserve its owner");
    qApp->removeEventFilter(&inputTrace);
    delayed(image);
    translation_tests::flushEvents();
    require(groups.liveWindows().size() == 1,
            "late loading completion must not revive hidden peers");
    Access::finishShakeDrag(*source);
    service.showAllWindows();
    translation_tests::waitUntil([&] { return groups.liveWindow(peerId) != nullptr; },
                                 "Show All must recover the hidden peer");
    const auto restored = repository.loadRecord(peerId);
    require(restored && !restored->hidden && !restored->ignored &&
                restored->nativeGeometry == before.nativeGeometry &&
                restored->canvasSession == before.canvasSession,
            "Show All must retain the hidden document and geometry");
    for (auto* window : groups.liveWindows())
        window->close();
    translation_tests::flushEvents();
}
} // namespace pinned_shake_tests

void pinnedShakeSamplingBenchmark() {
    using namespace pinned_shake_tests;
    IsolatedPinnedStorage isolated;
    ScreenshotPinnedWindow window;
    present(window, config());
    constexpr int iterations = 250000;
    std::array<double, 9> enabled{};
    std::array<double, 9> disabled{};
    for (const bool setting : {false, true}) {
        require(Settings().setShakeWindowToHideOthers(setting), "set benchmark preference");
        auto& samples = setting ? enabled : disabled;
        for (auto& sample : samples) {
            require(Access::beginShakeDrag(window), "begin sampling benchmark drag");
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < iterations; ++i)
                Access::sampleShakeRuntime(window, QPointF(i, i % 3));
            sample = double(timer.nsecsElapsed()) / iterations;
            Access::finishShakeDrag(window);
            require(!Access::shakeMessage(window), "ordinary movement must not hide peers");
        }
        std::sort(samples.begin(), samples.end());
    }
    std::cout << "Shake drag sampling, ns/sample (median / worst trial): disabled " << disabled[4]
              << " / " << disabled.back() << ", enabled " << enabled[4] << " / " << enabled.back()
              << ", added median " << enabled[4] - disabled[4] << '\n';
    window.close();
}

void pinnedShakeWindowTests() {
    using namespace pinned_shake_tests;
    ordinaryDragCorrectionsKeepPeersVisible();
    callbackAndMessagePreserveDrag();
    pendingActionsRespectCancellationAndSettings();
    liveSettingAndResetsRequireAFreshDrag();
    failuresAndEmptyTargetsDoNotShowSuccess();
    fallbackHidesOnlyPeersAndPreservesSnapshots();
    dpiChangesAndSharedGeometryCannotCompleteOldShakes();
    peerContentAndWindowModesSurviveHiding();
    serviceHidesPersistedAndPendingPeers();
}

#ifdef Q_OS_WIN
void pinnedShakeInsideRealMoveLoop() {
    using namespace pinned_shake_tests;
    const CursorPositionRestorer restorePointer;
    ScreenshotPinnedWindow peer;
    ScreenshotPinnedWindow window;
    present(peer, config());
    bool hiddenInsideMoveLoop = false;
    auto value = config();
    value.shakeHideOtherWindowsRequested = [&](const QString&) {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        hiddenInsideMoveLoop = GetGUIThreadInfo(GetCurrentThreadId(), &info) != FALSE &&
                               (info.flags & GUI_INMOVESIZE) != 0 &&
                               info.hwndMoveSize == reinterpret_cast<HWND>(window.winId());
        const bool hidden = peer.requestHide();
        return Result{hidden, hidden};
    };
    present(window, value);
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const QPoint origin = window.currentNativeGeometry().center();
    setSystemCursorPosition(origin);
    struct Probe {
        QPoint origin;
        int amplitude;
        int tick = 0;
        ScreenshotPinnedWindow* owner;
        ScreenshotPinnedWindow* peer;
        bool continuedDragging = false;
        bool inputDelivered = true;
        bool started = false;
        bool seeded = false;
        std::array<QString, 8> states;
        QStringList nativeEvents;
    } probe{origin, qRound(64 * window.screen()->devicePixelRatio()), 0, &window, &peer};
    class Trace final : public QAbstractNativeEventFilter {
      public:
        Trace(Probe* value, HWND handle) : probe(value), hwnd(handle) {
            clock.start();
        }
        bool nativeEventFilter(const QByteArray&, void* raw, qintptr*) override {
            const auto* message = static_cast<const MSG*>(raw);
            if (message && message->hwnd == hwnd &&
                (message->message == WM_ENTERSIZEMOVE || message->message == WM_EXITSIZEMOVE ||
                 message->message == WM_MOVING))
                probe->nativeEvents.append(QStringLiteral("native=%1 at=%2 %3")
                                               .arg(message->message)
                                               .arg(clock.elapsed())
                                               .arg(Access::shakeDiagnostics(*probe->owner)));
            return false;
        }
        Probe* probe;
        HWND hwnd;
        QElapsedTimer clock;
    } trace(&probe, hwnd);
    qApp->installNativeEventFilter(&trace);
    const auto removeTrace = qScopeGuard([&] { qApp->removeNativeEventFilter(&trace); });
    require(SetPropW(hwnd, L"SnowPinnedShakeProbe", &probe) != FALSE, "install real shake probe");
    const UINT_PTR timer =
        SetTimer(hwnd, 0x5353484B, 60, [](HWND timerWindow, UINT, UINT_PTR timerId, DWORD) {
            auto* state = static_cast<Probe*>(GetPropW(timerWindow, L"SnowPinnedShakeProbe"));
            const auto movePointer = [state](const QPoint& target) {
                INPUT move{};
                move.type = INPUT_MOUSE;
                move.mi.dx = MulDiv(target.x() - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535,
                                    std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
                move.mi.dy = MulDiv(target.y() - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535,
                                    std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
                move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                                  MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
                state->inputDelivered &= SendInput(1, &move, sizeof(move)) == 1;
            };
            GUITHREADINFO info{};
            info.cbSize = sizeof(info);
            if (GetGUIThreadInfo(GetCurrentThreadId(), &info) == FALSE ||
                (info.flags & GUI_INMOVESIZE) == 0 || info.hwndMoveSize != timerWindow) {
                if (!state->seeded) {
                    state->seeded = true;
                    movePointer(state->origin + QPoint(state->amplitude / 2, 0));
                }
                return;
            }
            if (!state->started) {
                state->started = true;
                state->origin = systemCursorPosition();
            }
            ++state->tick;
            if (state->tick <= 8)
                state->states[static_cast<std::size_t>(state->tick - 1)] =
                    Access::shakeDiagnostics(*state->owner);
            if (state->tick <= 6) {
                const QPoint target =
                    state->origin + QPoint((state->tick % 2) ? state->amplitude : 0, 0);
                movePointer(target);
                return;
            }
            if (state->tick == 7) {
                state->continuedDragging =
                    !state->peer->isVisible() && Access::previewNativeMoveActive(*state->owner);
                movePointer(state->origin + QPoint(15, 8));
                return;
            }
            KillTimer(timerWindow, timerId);
            INPUT release{};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &release, sizeof(release));
            PostMessageW(timerWindow, WM_LBUTTONUP, 0, 0);
        });
    require(timer != 0, "install native shake driver timer");
    const auto cleanup = qScopeGuard([&] {
        KillTimer(hwnd, timer);
        RemovePropW(hwnd, L"SnowPinnedShakeProbe");
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &release, sizeof(release));
    });
    require(WindowFromPoint(POINT{origin.x(), origin.y()}) == hwnd,
            "real shake input must target its own pin");
    INPUT press{};
    press.type = INPUT_MOUSE;
    press.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    require(SendInput(1, &press, sizeof(press)) == 1, "press real shake drag button");
    // Let the real press enter the normal Qt/USER32 route exactly once.
    waitForUi(1000);
    if (!hiddenInsideMoveLoop || !probe.continuedDragging || peer.isVisible() ||
        !Access::shakeMessage(window)) {
        std::cerr << "Real shake: hidden in loop=" << hiddenInsideMoveLoop
                  << " continued=" << probe.continuedDragging
                  << " peer visible=" << peer.isVisible()
                  << " message=" << bool(Access::shakeMessage(window)) << '\n';
        for (const auto& state : probe.states)
            std::cerr << state.toStdString() << '\n';
        for (const auto& state : probe.nativeEvents)
            std::cerr << state.toStdString() << '\n';
    }
    require(probe.inputDelivered && hiddenInsideMoveLoop && probe.continuedDragging &&
                !peer.isVisible() && Access::shakeMessage(window),
            "queued hide and feedback must execute inside the real native move loop");
    window.close();
}

void pinnedShakeNativeTests() {
    using namespace pinned_shake_tests;
    IsolatedPinnedStorage isolated;
    for (const QString scenario :
         {QStringLiteral("success"), QStringLiteral("release"), QStringLiteral("escape"),
          QStringLiteral("capture-loss"), QStringLiteral("keyboard"),
          QStringLiteral("enable-during-handoff"), QStringLiteral("toggle-during-handoff"),
          QStringLiteral("dpi-change")}) {
        require(Settings().setShakeWindowToHideOthers(true), "reset native shake preference");
        QPoint pointer;
        ScreenshotPinnedWindow window;
        ScreenshotPinnedWindow peer;
        present(peer, config());
        auto value = config();
        int requests = 0;
        value.shakeHideOtherWindowsRequested = [&](const QString&) {
            ++requests;
            const bool hidden = peer.requestHide();
            return Result{hidden, hidden};
        };
        present(window, value);
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        const QRect original = window.currentNativeGeometry();
        const QPoint origin = original.center();
        pointer = origin;
        Access::setPhysicalCursor(window, {true, [&] { return std::optional<QPoint>(pointer); },
                                           [&](const QPoint& value) {
                                               pointer = value;
                                               return true;
                                           }});
        withNativeLeftButtonDown([&] {
            if (scenario == u"enable-during-handoff")
                require(Settings().setShakeWindowToHideOthers(false),
                        "disable before native press");
            require(Access::startNativeShakeDrag(window), "begin native system shake drag");
            discardPostedSystemDrag(hwnd);
            if (scenario == u"toggle-during-handoff")
                require(Settings().setShakeWindowToHideOthers(false), "disable native handoff");
            if (scenario == u"enable-during-handoff" || scenario == u"toggle-during-handoff")
                require(Settings().setShakeWindowToHideOthers(true),
                        "enable before native ownership");
            Access::previewNativeMessage(window, WM_ENTERSIZEMOVE);
            if (scenario == u"keyboard")
                sendShortcut(window, Qt::Key_W);
            const int amplitude = qRound(64 * window.screen()->devicePixelRatio());
            for (int leg = 1; leg <= 6; ++leg) {
                if (scenario == u"dpi-change" && leg == 3) {
                    MSG dpi{};
                    dpi.hwnd = hwnd;
                    dpi.message = WM_DPICHANGED;
                    const WORD scale = static_cast<WORD>(GetDpiForWindow(hwnd) + 24);
                    dpi.wParam = MAKELONG(scale, scale);
                    qintptr result = 0;
                    static_cast<void>(PinnedWindowWindowsEvents::handle(
                        window, QByteArrayLiteral("windows_generic_MSG"), &dpi, &result));
                }
                const QPoint delta((leg % 2) ? amplitude : 0, 0);
                pointer = origin + delta;
                RECT proposal = nativeRectForQRect(original.translated(delta));
                Access::previewNativeMessage(window, WM_MOVING,
                                             reinterpret_cast<LPARAM>(&proposal));
                SetWindowPos(hwnd, nullptr, proposal.left, proposal.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (scenario == u"escape") {
                MSG key{};
                key.hwnd = hwnd;
                key.message = WM_KEYDOWN;
                key.wParam = VK_ESCAPE;
                qintptr result = 0;
                static_cast<void>(PinnedWindowWindowsEvents::handle(
                    window, QByteArrayLiteral("windows_generic_MSG"), &key, &result));
            } else if (scenario == u"capture-loss")
                Access::previewNativeMessage(window, WM_CANCELMODE);
            else if (scenario == u"release")
                Access::previewNativeMessage(window, WM_EXITSIZEMOVE);
            QCoreApplication::processEvents();
            const bool expected = scenario == u"success" || scenario == u"release";
            if (requests != (expected ? 1 : 0) || peer.isVisible() == expected ||
                bool(Access::shakeMessage(window)) != expected)
                std::cerr << "Native scenario " << scenario.toStdString()
                          << " requests=" << requests << " peer visible=" << peer.isVisible()
                          << " message=" << bool(Access::shakeMessage(window)) << ' '
                          << Access::shakeDiagnostics(window).toStdString() << '\n';
            require(
                requests == (expected ? 1 : 0) && peer.isVisible() != expected &&
                    bool(Access::shakeMessage(window)) == expected,
                "native shake must hide peers while cancellation and keyboard moves stay inert");
            if (scenario == u"success")
                require(Access::previewNativeMoveActive(window),
                        "native move must survive hiding peers");
            Access::previewNativeMessage(window, WM_EXITSIZEMOVE);
        });
        window.close();
        peer.close();
    }
    pinnedShakeInsideRealMoveLoop();
}
#endif
