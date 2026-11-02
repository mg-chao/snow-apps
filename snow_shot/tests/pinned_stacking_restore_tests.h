// Included after the shared pinned-window storage and presentation fixtures.
void pinnedStackingSurvivesRestoration() {
    IsolatedPinnedStorage isolated;
    auto& store = snow_shot::storage::ApplicationStorage::instance();
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen, "stacking restore requires a display");
    require(snow_shot::storage::PinToScreenSettings().setAutomaticTextRecognition(false),
            "disable recognition in stacking fixture");
    QStringList ids;
    for (int i = 0; i < 3; ++i) {
        auto record =
            savedPinnedRecord(*screen, 1.0, QSize(120 + 10 * i, 80), 100.0, QPoint(40, 40));
        ids.append(record.id);
        require(store.pinnedWindows().upsert(record).success, "seed restart source");
    }
    QStringList expected{ids.at(1), ids.at(2), ids.at(0)};
    require(store.pinnedWindows().updateStackingOrder("default", expected).success &&
                store.flushNow().success,
            "persist initial stack");
    auto platform = snow_shot::presentation::createPinnedWindowStackingPlatform();
    const auto wait = [](const auto& predicate, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThread::msleep(1);
        }
        require(predicate(), message);
        QCoreApplication::processEvents();
    };
    for (int session = 0; session < 2; ++session) {
        {
            snow_shot::presentation::PinnedWindowGroupManager groups(&store.pinnedWindows());
            ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {},
                                                         &groups);
            auto* stacking = groups.stackingCoordinator();
            QObject connections;
            QObject::connect(&store, &snow_shot::storage::ApplicationStorage::checkpointRequested,
                             stacking,
                             &snow_shot::presentation::PinnedWindowStackingCoordinator::checkpoint);
            QObject::connect(&groups,
                             &snow_shot::presentation::PinnedWindowGroupManager::
                                 restoreActiveGroupWindowsRequested,
                             &connections, [&] {
                                 QTimer::singleShot(0, &connections,
                                                    [&] { services.restorePersistedWindows(); });
                             });
#if defined(Q_OS_WIN)
            const auto foreground = GetForegroundWindow();
#endif
            const auto ready = [&] {
                for (const auto& id : ids) {
                    auto* window = groups.liveWindow(id);
                    if (!window ||
                        !window->automationState().value(QStringLiteral("ready")).toBool())
                        return false;
                }
                return true;
            };
            const auto retired = [&] {
                for (const auto& id : ids)
                    if (groups.liveWindow(id))
                        return false;
                return true;
            };
            const auto verify = [&] {
                require(store.pinnedWindows().stackingOrder("default") == expected,
                        "restoration and late state saves retain desired order");
                snow_shot::presentation::PinnedStackingWindows candidates;
                for (const auto& id : expected) {
                    auto* window = groups.liveWindow(id);
                    require(window && window->testAttribute(Qt::WA_ShowWithoutActivating),
                            "automatic presentation uses preserve-focus policy");
                    candidates.append(window);
                }
                if (platform.capture) {
                    const auto actual = platform.capture(candidates);
                    require(actual && *actual == candidates,
                            "restored native pin stack matches persisted IDs");
                }
#if defined(Q_OS_WIN)
                if (platform.capture)
                    require(GetForegroundWindow() == foreground,
                            "automatic pin restore leaves external focus intact");
#endif
            };
            services.restorePersistedWindows();
            wait(ready, "restore all pins from disk");
            verify();
            if (session == 0) {
                // A real reorder without activation followed by an unrelated state save.
                expected = {ids.at(2), ids.at(0), ids.at(1)};
                if (platform.capture) {
                    for (auto it = expected.crbegin(); it != expected.crend(); ++it)
                        groups.liveWindow(*it)->raise();
                } else {
                    for (auto it = expected.crbegin(); it != expected.crend(); ++it)
                        require(store.pinnedWindows().promoteStackingWindow(*it).success,
                                "offscreen explicit reorder");
                }
                groups.liveWindow(ids.first())->checkpointStorage();
                require(store.flushNow().success, "pre-flush hook captures actual stacking order");
                verify();
                require(services.toggleAllWindowsVisibility(), "hide all pins");
                wait(retired, "hidden pins retire");
                services.showAllWindows();
                wait(ready, "show-all restores pins");
                verify();
                const auto group = groups.createGroup(QStringLiteral("Stacking test"));
                require(group && groups.setActiveGroup(*group), "switch away from pinned group");
                wait(retired, "inactive group retires pins");
                require(groups.setActiveGroup(QStringLiteral("default")),
                        "switch back to saved group");
                wait(ready, "group restore recreates pins");
                verify();
            }
            stacking->beginMutation();
            for (auto* window : groups.liveWindows())
                window->closeForInactiveGroup();
            stacking->endMutation();
            wait(retired, "retire session windows before repository restart");
            stacking->shutdown();
            require(store.flushNow().success, "flush before repository restart");
        }
        if (session == 0) {
            const QString directory = store.configurationDirectory();
            store.shutdown();
            require(
                store.initialize({QDir(directory).filePath(QStringLiteral("../bin")), directory, 0})
                    .success,
                "restart application storage from persisted files");
        }
    }
}

void pinnedStackingExplicitShowDuringRestore() {
    IsolatedPinnedStorage isolated;
    auto& store = snow_shot::storage::ApplicationStorage::instance();
    auto* screen = QGuiApplication::primaryScreen();
    require(screen, "explicit restore requires a display");
    require(snow_shot::storage::PinToScreenSettings().setAutomaticTextRecognition(false),
            "disable recognition in explicit restore fixture");
    QStringList ids;
    for (int i = 0; i < 3; ++i) {
        auto record = savedPinnedRecord(*screen, 1.0, QSize(120, 80), 100.0, QPoint(40, 40));
        ids.append(record.id);
        require(store.pinnedWindows().upsert(record).success, "seed delayed restore");
    }
    require(store.pinnedWindows().updateStackingOrder("default", ids).success,
            "seed delayed order");
    snow_shot::presentation::PinnedWindowGroupManager groups(&store.pinnedWindows());
    ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {}, &groups);
    QSemaphore started, release;
    store.pinnedFullImagePool().start([&] {
        started.release();
        release.acquire();
    });
    auto unblock = qScopeGuard([&] {
        release.release();
        store.pinnedFullImagePool().waitForDone();
    });
    require(started.tryAcquire(1, 5000), "block image loader deterministically");
    {
        ScreenshotSelectionExportUiServices canceled(nullptr, nullptr, nullptr, {}, {}, &groups);
        canceled.restorePersistedWindows();
    }
    // Old service callbacks remain queued behind the gate while replacements reuse the IDs.
    services.restorePersistedWindows();
    require(services.restoreRecord(ids.at(1)) && services.restoreRecord(ids.at(2)),
            "explicit shows upgrade existing pending restores");
    require(store.pinnedWindows().stackingOrder("default").first() == ids.at(2),
            "explicit show records foreground intent before decoding");
    release.release();
    unblock.dismiss();
    QElapsedTimer timer;
    timer.start();
    const auto ready = [&] {
        for (const auto& id : ids) {
            auto* window = groups.liveWindow(id);
            if (!window || !window->automationState().value(QStringLiteral("ready")).toBool())
                return false;
        }
        return true;
    };
    while (!ready() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "delayed explicit restores complete");
    QCoreApplication::processEvents();
    require(store.pinnedWindows().stackingOrder("default").first() == ids.at(2) &&
                groups.liveWindow(ids.at(1))->testAttribute(Qt::WA_ShowWithoutActivating) &&
                !groups.liveWindow(ids.at(2))->testAttribute(Qt::WA_ShowWithoutActivating),
            "only the latest explicit request activates after loading");
    const auto platform = snow_shot::presentation::createPinnedWindowStackingPlatform();
    if (platform.capture) {
        snow_shot::presentation::PinnedStackingWindows desired;
        for (const auto& id : store.pinnedWindows().stackingOrder("default"))
            desired.append(groups.liveWindow(id));
        const auto actual = platform.capture(desired);
        require(actual && *actual == desired, "late automatic completions cannot cover selection");
    }
    groups.stackingCoordinator()->beginMutation();
    for (auto* window : groups.liveWindows())
        window->closeForInactiveGroup();
    groups.stackingCoordinator()->endMutation();
    groups.stackingCoordinator()->shutdown();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void pinnedStackingRestoreDuringMutation() {
    IsolatedPinnedStorage isolated;
    auto& store = snow_shot::storage::ApplicationStorage::instance();
    auto* screen = QGuiApplication::primaryScreen();
    require(screen, "suspended restore requires a display");
    require(snow_shot::storage::PinToScreenSettings().setAutomaticTextRecognition(false),
            "disable recognition in suspended restore fixture");
    QStringList ids;
    for (int i = 0; i < 3; ++i) {
        auto record = savedPinnedRecord(*screen, 1.0, QSize(120, 80), 100.0, QPoint(40, 40));
        ids.append(record.id);
        require(store.pinnedWindows().upsert(record).success, "seed suspended restore");
    }
    require(store.pinnedWindows().updateStackingOrder("default", ids).success,
            "seed suspended order");
    snow_shot::presentation::PinnedWindowGroupManager groups(&store.pinnedWindows());
    ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {}, &groups);
    QSemaphore started, release;
    store.pinnedFullImagePool().start([&] {
        started.release();
        release.acquire();
    });
    auto unblock = qScopeGuard([&] {
        release.release();
        store.pinnedFullImagePool().waitForDone();
    });
    require(started.tryAcquire(1, 5000), "gate restoration before suspension");
    services.restorePersistedWindows();
    auto* stacking = groups.stackingCoordinator();
    stacking->beginMutation();
    release.release();
    unblock.dismiss();
    QElapsedTimer timer;
    timer.start();
    const auto ready = [&] {
        for (const auto& id : ids) {
            const auto* window = groups.liveWindow(id);
            if (!window || !window->automationState().value(QStringLiteral("ready")).toBool() ||
                store.pinnedWindows().summary(id)->ignored)
                return false;
        }
        return true;
    };
    while (!ready() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "presentations finish while native stacking is suspended");
    QCoreApplication::processEvents();
    stacking->endMutation();
    QCoreApplication::processEvents();
    require(store.pinnedWindows().stackingOrder("default") == ids,
            "resuming automatic restore retains saved intent");
    const auto platform = snow_shot::presentation::createPinnedWindowStackingPlatform();
    if (platform.capture) {
        snow_shot::presentation::PinnedStackingWindows desired;
        for (const auto& id : ids)
            desired.append(groups.liveWindow(id));
        const auto actual = platform.capture(desired);
        require(actual && *actual == desired, "resuming reconciles every restored native window");
    }
    groups.liveWindow(ids.last())->showFromManagement();
    require(store.pinnedWindows().stackingOrder("default").first() == ids.last(),
            "management activation still tracks a pin presented during suspension");
    services.destroyRecords(ids);
    stacking->shutdown();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
