// Included after the shared pinned-window storage and presentation fixtures.
namespace pinned_runtime_settings_tests {
void waitUntil(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        translation_tests::flushEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

void verifyWheelAnchor(ScreenshotPinnedWindow& window, const QString& mode) {
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    require(canvas, "runtime settings pin requires a canvas");
    const QRect before = window.currentNativeGeometry();
    const double scale = ScreenshotPinnedWindowTestAccess::scale(window);
    const QPoint position(canvas->width() / 3, canvas->height() / 3);
    QWheelEvent wheel(QPointF(position), QPointF(canvas->mapToGlobal(position)), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);
    const QRect after = window.currentNativeGeometry();
    require(wheel.isAccepted() && ScreenshotPinnedWindowTestAccess::scale(window) == scale + 10 &&
                after.width() > before.width() && after.height() > before.height(),
            "a restored pin must continue to handle wheel zoom");
    if (mode == QStringLiteral("top_left")) {
        require(after.topLeft() == before.topLeft(),
                "restoration must preserve the configured top-left wheel anchor");
    } else if (mode == QStringLiteral("top_right")) {
        require(after.topRight() == before.topRight(),
                "restoration must preserve the configured top-right wheel anchor");
    } else if (mode == QStringLiteral("bottom_left")) {
        require(after.bottomLeft() == before.bottomLeft(),
                "restoration must preserve the configured bottom-left wheel anchor");
    } else if (mode == QStringLiteral("bottom_right")) {
        require(after.bottomRight() == before.bottomRight(),
                "restoration must preserve the configured bottom-right wheel anchor");
    } else if (mode == QStringLiteral("center")) {
        require((after.center() - before.center()).manhattanLength() <= 1,
                "restoration must preserve the configured center wheel anchor");
    } else {
        require(after.topLeft() != before.topLeft() && after.topRight() != before.topRight() &&
                    after.bottomLeft() != before.bottomLeft() &&
                    after.bottomRight() != before.bottomRight() &&
                    (after.center() - before.center()).manhattanLength() > 1,
                "restoration must retain mouse-position zoom at an off-center pointer");
    }
}

void exerciseRestorationPaths(const QString& mode, bool verifyZoom) {
    IsolatedPinnedStorage isolated;
    using namespace snow_shot;
    auto& store = storage::ApplicationStorage::instance();
    auto& repository = store.pinnedWindows();
    const storage::PinToScreenSettings settings;
    require(settings.setMouseWheelZoomMode(mode) && settings.setMouseWheelZoomStep(10) &&
                settings.setAutomaticTextRecognition(false),
            "configure non-default pinned runtime preferences");
    presentation::PinnedWindowGroupManager groups(&repository);
    const auto otherGroup = groups.createGroup(QStringLiteral("Runtime settings"));
    require(otherGroup.has_value(), "create an alternate pinned group");
    IdleOcrRecognition recognition;
    ScreenshotSelectionExportUiServices services(&recognition, nullptr, nullptr, {}, {}, &groups);
    QObject connections;
    QObject::connect(&store, &storage::ApplicationStorage::pinnedWindowsChanged, &connections,
                     [&] { groups.onPinnedRecordsChanged(); });
    QObject::connect(
        &groups, &presentation::PinnedWindowGroupManager::restoreActiveGroupWindowsRequested,
        &connections,
        [&] { QTimer::singleShot(0, &connections, [&] { services.restorePersistedWindows(); }); });
    const auto cleanup = qScopeGuard([&] {
        for (auto* window : groups.liveWindows())
            window->closeForInactiveGroup();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen, "runtime settings restoration requires a display");
    QImage image(320, 180, QImage::Format_RGB32);
    image.fill(Qt::blue);
    require(services.presentPinnedImage(image, screen,
                                        physicalPinGeometry(*screen, {120, 100}, image.size()),
                                        image.size()),
            "create a pin through the normal application service");
    waitUntil([&] { return repository.summaries().size() == 1; },
              "the new pin must persist before switching groups");
    const QString id = repository.summaries().front().id;
    int expectedRequests = 0;
    const auto verify = [&](const char* message) {
        waitUntil(
            [&] {
                auto* window = groups.liveWindow(id);
                return window && window->isVisible() &&
                       window->automationState().value(QStringLiteral("ready")).toBool() &&
                       ScreenshotPinnedWindowTestAccess::recognitionReady(*window);
            },
            message);
        auto* window = groups.liveWindow(id);
        if (verifyZoom) {
            verifyWheelAnchor(*window, settings.mouseWheelZoomMode());
        } else {
            if (settings.automaticTextRecognition()) {
                ++expectedRequests;
                waitUntil([&] { return recognition.requests == expectedRequests; },
                          "enabled automatic recognition must run after restore");
            }
            // Drain the zero-delay prefetch queued by deferred presentation setup.
            translation_tests::flushEvents();
            translation_tests::flushEvents();
            require(recognition.requests == expectedRequests,
                    "restoration must respect disabled automatic text recognition");
        }
    };
    const auto retire = [&](bool userClose) {
        QPointer<ScreenshotPinnedWindow> window(groups.liveWindow(id));
        require(window, "the pin must exist before retiring it");
        if (userClose) {
            auto* closeAction =
                pinnedMenuActionNamed(*window, QStringLiteral("screenshotPinnedCloseAction"));
            require(closeAction, "the pin must provide the user Close action");
            closeAction->trigger();
        } else {
            window->closeForInactiveGroup();
        }
        require(processUntilDeleted(window, 2000), "the old pin must retire before recreation");
        const auto saved = repository.summary(id);
        require(saved && saved->ignored == userClose,
                "retiring the pin must preserve the appropriate closed-history state");
    };
    verify("the new pin must finish presentation");

    QPointer<ScreenshotPinnedWindow> original(groups.liveWindow(id));
    require(groups.setActiveGroup(*otherGroup), "switch away from the pinned group");
    require(processUntilDeleted(original, 2000), "group switching must retire the original pin");
    require(groups.setActiveGroup(QStringLiteral("default")), "return to the pinned group");
    verify("group switching must finish restoring the pin");

    original = groups.liveWindow(id);
    require(services.toggleAllWindowsVisibility(), "hide the pin before restoring visibility");
    require(processUntilDeleted(original, 2000), "hiding must retire the pin");
    require(settings.setAutomaticTextRecognition(!verifyZoom), "update recognition while hidden");
    services.showAllWindows();
    verify("showing a hidden pin must finish restoration");

    retire(true);
    require(settings.setAutomaticTextRecognition(false), "disable recognition before reopening");
    services.restoreLastClosedWindow();
    verify("reopening the last closed pin must finish restoration");

    retire(true);
    require(settings.setAutomaticTextRecognition(!verifyZoom), "update recognition before restore");
    require(services.restoreRecord(id), "restore a pin from window management");
    verify("window management must finish restoring the pin");

    retire(true);
    require(settings.setAutomaticTextRecognition(false),
            "disable recognition before batch restore");
    require(services.restoreRecords({id}), "restore a pin through batch window management");
    verify("batch window management must finish restoring the pin");

    retire(false);
    require(settings.setMouseWheelZoomMode(mode == QStringLiteral("top_left")
                                               ? QStringLiteral("bottom_right")
                                               : QStringLiteral("top_left")),
            "change the anchor before restoring saved windows");
    // Startup uses this same entry point for all non-closed pins in the active group.
    services.restorePersistedWindows();
    verify("startup must finish restoring the pin");
}

void zoom() {
    for (const QString& mode : {QStringLiteral("top_left"), QStringLiteral("top_right"),
                                QStringLiteral("bottom_left"), QStringLiteral("bottom_right"),
                                QStringLiteral("center"), QStringLiteral("mouse_position")}) {
        exerciseRestorationPaths(mode, true);
    }
}

void recognition() {
    exerciseRestorationPaths(QStringLiteral("mouse_position"), false);
}
} // namespace pinned_runtime_settings_tests
