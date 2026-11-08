// Included after the shared pinned-window persistence fixtures.
void persistedDuplicateLifecycleActions() {
    IsolatedPinnedStorage isolated;
    using namespace snow_shot;
    auto& repository = storage::ApplicationStorage::instance().pinnedWindows();
    require(storage::PinToScreenSettings().setAutomaticTextRecognition(false), "disable OCR");
    presentation::PinnedWindowGroupManager groups(&repository);
    ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {}, &groups);
    auto* screen = QGuiApplication::primaryScreen();
    require(screen, "duplicate lifecycle fixture requires a screen");
    const auto wait = [](auto predicate, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThread::msleep(1);
        }
        require(predicate(), message);
    };
    const storage::PinnedSourceIdentity identity{QStringLiteral("file:persisted-visible.png")};
    auto older = savedPinnedRecord(*screen, 1, {100, 60}, 100, {40, 40});
    older.sourceIdentity = identity;
    older.createdUtc = QDateTime::currentDateTimeUtc().addSecs(-10);
    auto newer = older;
    newer.id = QStringLiteral("00000000-0000-0000-0000-000000000001");
    newer.createdUtc = older.createdUtc.addSecs(1);
    newer.hidden = true;
    SnowCanvasRuntime annotations;
    require(!annotations
                 .applyAnnotationTransaction(
                     R"({"version":1,"operations":[{"type":"rectangle","bounds":[8,8,24,24]}]})")
                 .isEmpty(),
            "retained duplicate has independent annotations");
    newer.canvasSession = annotations.serializeDocumentSession();
    auto tie = newer;
    tie.id = QStringLiteral("00000000-0000-0000-0000-000000000002");
    tie.hidden = false;
    auto ignored = newer;
    ignored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ignored.createdUtc = newer.createdUtc.addSecs(1);
    ignored.ignored = true;
    ignored.hidden = false;
    ignored.lastClosedUtc = ignored.createdUtc;
    // Raw ImageData fixtures use upsert; create requires an admitted PreparedPngImage.
    // Initial insertion reveals group members, so apply the hidden state after all seeds.
    require(repository.upsert(older).success && repository.upsert(newer).success &&
                repository.upsert(tie).success && repository.upsert(ignored).success &&
                repository.setRecordsHidden({newer.id}, true).success && repository.flush().success,
            "seed visible, hidden, ignored and equal-time retained duplicate sources");
    require(!services.findDuplicatePin(identity) &&
                services.duplicateSourceKeys().contains(identity.key),
            "persisted visible records participate before any shell or restore exists");
    bool restored = false;
    require(services.handleDuplicatePin(identity, QStringLiteral("none"), restored) && !restored &&
                repository.summaries().size() == 4 && !services.findDuplicatePin(identity),
            "None consumes a stored duplicate without creating or showing a shell");
    require(!services.handleDuplicatePin(identity, QStringLiteral("repeat_action"), restored),
            "Repeat Action retains intentional independent creation");
    require(services.handleDuplicatePin(identity, QStringLiteral("shake_window"), restored) &&
                services.handleDuplicatePin(identity, QStringLiteral("shake_window"), restored),
            "repeated attention joins the existing restore reservation");
    wait(
        [&] {
            auto* window = services.findDuplicatePin(identity);
            return window && window->findChild<QVariantAnimation*>(
                                 QStringLiteral("screenshotPinnedShakeAnimation"));
        },
        "the newest eligible private snapshot restores once and receives attention");
    auto* window = services.findDuplicatePin(identity);
    SnowCanvasRuntime restoredAnnotations;
    require(window->persistenceId() == newer.id &&
                restoredAnnotations.restoreDocumentSession(
                    window->persistenceSnapshot().canvasSession) &&
                restoredAnnotations.documentCheckpoint() == annotations.documentCheckpoint() &&
                restoredAnnotations.canUndo() && repository.summaries().size() == 4 &&
                repository.loadRecord(ignored.id)->ignored,
            "stable ID resolves timestamp ties, ignored pins stay excluded and edits survive");
    const auto other = groups.createGroup(QStringLiteral("Other pins"));
    require(other.has_value(), "create a valid duplicate-policy group");
    require(groups.setActiveGroup(*other), "switch duplicate-policy group");
    require(!services.duplicateSourceKeys().contains(identity.key) &&
                !services.handleDuplicatePin(identity, QStringLiteral("none"), restored),
            "duplicate discovery and consumption share active-group boundaries");
    require(groups.setActiveGroup(QStringLiteral("default")), "return to duplicate source group");
    restored = false;
    require(services.handleDuplicatePin(identity, QStringLiteral("restore_last_closed_window"),
                                        restored) &&
                restored &&
                services.handleDuplicatePin(identity, QStringLiteral("restore_last_closed_window"),
                                            restored),
            "last-closed action consumes all duplicates with one batch restoration");
    wait([&] { return !repository.loadRecord(ignored.id)->ignored; },
         "last-closed policy restores its existing ignored record");
    QVector<QString> ids;
    for (const auto& record : repository.summaries())
        ids.append(record.id);
    services.destroyRecords(ids);
    wait([&] { return !services.findDuplicatePin(identity); }, "retire duplicate fixtures");
}
