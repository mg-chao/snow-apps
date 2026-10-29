#ifndef SNOW_SHOT_TESTS_PINNED_TEXT_TYPE_TESTS_H
#define SNOW_SHOT_TESTS_PINNED_TEXT_TYPE_TESTS_H

namespace {
QAction* pinnedTextTypeAction(ScreenshotPinnedWindow& window, const char* name) {
    materializePinnedMenuTree(window);
    auto* action = window.findChild<QAction*>(QString::fromLatin1(name));
    require(action != nullptr, "the pinned text type action exists");
    return action;
}

void pinnedThumbnailExitsTextDisplay() {
    using Access = ScreenshotPinnedWindowTestAccess;
    using snow_shot::storage::PinnedTextType;
    for (const auto textType :
         {PinnedTextType::RecognitionResults, PinnedTextType::TranslationResults}) {
        for (const bool animate : {false, true}) {
            IdleOcrRecognition recognition;
            auto config = cachedOcrPinConfig(&recognition);
            auto translated = std::make_shared<ScreenshotOcrPresentation>();
            translated->selection = config.recognitionResults.text->presentation->selection;
            translated->lines = config.recognitionResults.text->presentation->lines;
            translated->setLineText(0, QStringLiteral("Saved translation"));
            translated->prepareForRendering();
            config.recognitionResults.translatedText = translated;
            ScreenshotPinnedWindow window;
            auto* session = Access::hiddenSelectionOffscreen(window, config);
            auto* display = pinnedTextTypeAction(window, "screenshotPinnedOcrAction");
            auto* thumbnail = pinnedTextTypeAction(window, "screenshotPinnedThumbnailAction");
            pinnedTextTypeAction(window, textType == PinnedTextType::TranslationResults
                                             ? "screenshotPinnedTranslationResultsAction"
                                             : "screenshotPinnedRecognitionResultsAction")
                ->trigger();
            auto* canvas = window.findChild<SnowCanvasWidget*>();
            auto* content = window.findChild<ScreenshotRecognitionWindow*>();
            require(session->active() && display->isChecked() && canvas && content &&
                        !canvas->canvasContentVisible() && !content->isHidden() &&
                        window.findChild<ScreenshotPinnedEditController*>() == nullptr,
                    "the thumbnail fixture shows cached text without a drawing controller");
            if (animate) {
                thumbnail->trigger();
            } else {
                Access::thumbnailForAnimationTest(window, true, false);
            }
            const auto record = window.persistenceSnapshot();
            require(thumbnail->isChecked() && !session->active() && !display->isChecked() &&
                        canvas->canvasContentVisible() && content->isHidden() &&
                        !Access::hiddenSelection(window),
                    "entering thumbnail mode immediately exits text display and shows the image");
            require(record.thumbnailMode && !record.recognitionVisible &&
                        !record.translationVisible && record.textType == textType,
                    "thumbnail persistence clears text visibility and preserves the selected type");
            if (animate) {
                auto* animation = window.findChild<QVariantAnimation*>(
                    QStringLiteral("screenshotPinnedGeometryAnimation"));
                translation_tests::waitUntil(
                    [&] { return !animation || animation->state() != QAbstractAnimation::Running; },
                    "the thumbnail animation finishes");
            }
            Access::thumbnailForAnimationTest(window, false, false);
            require(!session->active() && !display->isChecked() && canvas->canvasContentVisible(),
                    "expanding the thumbnail keeps text display disabled");
            display->trigger();
            require(session->active() && display->isChecked() && recognition.requests == 0 &&
                        session->originalImageTranslationActive() ==
                            (textType == PinnedTextType::TranslationResults),
                    "text display can reopen the selected cached results after thumbnail mode");
        }
    }
}

void pinnedTextTypeMenuActivatesSelectedResults() {
    using Access = ScreenshotPinnedWindowTestAccess;
    using snow_shot::storage::PinnedTextType;
    IdleOcrRecognition recognition;
    auto config = cachedOcrPinConfig(&recognition);
    auto translated = std::make_shared<ScreenshotOcrPresentation>();
    translated->selection = config.recognitionResults.text->presentation->selection;
    translated->lines = config.recognitionResults.text->presentation->lines;
    translated->setLineText(0, QStringLiteral("Saved translation"));
    translated->prepareForRendering();
    config.recognitionResults.translatedText = translated;
    ScreenshotPinnedWindow window;
    auto* session = Access::hiddenSelectionOffscreen(window, config);
    auto* display = pinnedTextTypeAction(window, "screenshotPinnedOcrAction");
    auto* type = pinnedTextTypeAction(window, "screenshotPinnedTextTypeAction");
    auto* original = pinnedTextTypeAction(window, "screenshotPinnedRecognitionResultsAction");
    auto* translation = pinnedTextTypeAction(window, "screenshotPinnedTranslationResultsAction");
    auto* menu = Access::contextMenu(window);
    require(menu->actions().indexOf(type) == menu->actions().indexOf(display) + 1 &&
                display->text().section(QLatin1Char('\t'), 0, 0) == QStringLiteral("Show text") &&
                type->text() == QStringLiteral("Text type: Recognition results") &&
                menu->actionIcon(type) ==
                    snow_shot::presentation::icons::custom::outlined::TextType() &&
                original->text() == QStringLiteral("Recognition results") &&
                translation->text() == QStringLiteral("Translation results") &&
                type->menu()->actions() == QList<QAction*>{original, translation} &&
                original->actionGroup() == translation->actionGroup() &&
                original->actionGroup()->isExclusive() && original->isChecked() &&
                !translation->isChecked() && !display->isChecked(),
            "text type follows the display item and defaults exclusively to recognition");
    translation->trigger();
    require(session->active() && display->isChecked() && translation->isChecked() &&
                !original->isChecked() && recognition.requests == 0 &&
                type->text() == QStringLiteral("Text type: Translation results") &&
                window.persistenceSnapshot().textType == PinnedTextType::TranslationResults,
            "choosing translation while hidden immediately activates text display");
    require(session->originalImageTranslationActive() &&
                session->recognitionClipboardMimeData()->text() ==
                    QStringLiteral("Saved translation") &&
                window.findChild<ScreenshotPinnedEditController*>() == nullptr,
            "choosing translation restores captured results without a provider or toolbar");
    for (bool visible : {false, true, false, true}) {
        Access::toggleTextResults(window);
        require(session->active() == visible && display->isChecked() == visible &&
                    session->originalImageTranslationActive() == visible &&
                    translation->isChecked(),
                "the shared shortcut and mouse display toggle preserves the selected type");
    }
    window.activateWindow();
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "text type shortcut has a canvas receiver");
    canvas->setFocus();
    waitForUi(30);
    for (bool visible : {false, true}) {
        sendShortcut(*canvas, Qt::Key_D, Qt::ControlModifier);
        require(session->active() == visible && translation->isChecked() &&
                    session->originalImageTranslationActive() == visible,
                "the configured recognition shortcut uses the selected translation type");
    }
    const snow_shot::storage::PinToScreenSettings pinSettings;
    const QString previousMouseAction = pinSettings.middleMouseButtonAction();
    const auto restoreMouseAction =
        qScopeGuard([&] { pinSettings.setMiddleMouseButtonAction(previousMouseAction); });
    require(pinSettings.setMiddleMouseButtonAction(QStringLiteral("show_text_recognition_results")),
            "configure the mouse display toggle");
    const QPoint background(20, window.height() - 30);
    for (bool visible : {false, true}) {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(background),
                          QPointF(window.mapToGlobal(background)), Qt::MiddleButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(background),
                            QPointF(window.mapToGlobal(background)), Qt::MiddleButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(&window, &release);
        require(session->active() == visible && translation->isChecked() &&
                    session->originalImageTranslationActive() == visible,
                "the configured mouse display toggle uses the selected translation type");
    }
    display->trigger();
    require(!session->active() && !display->isChecked(), "text display can still be hidden");
    original->trigger();
    require(session->active() && !session->translating() && display->isChecked() &&
                original->isChecked() &&
                type->text() == QStringLiteral("Text type: Recognition results") &&
                session->recognitionClipboardMimeData()->text() == QStringLiteral("Saved OCR"),
            "choosing recognition while hidden immediately displays source OCR");
    display->trigger();
    original->trigger();
    require(session->active() && display->isChecked() && original->isChecked() &&
                recognition.requests == 0,
            "reselecting the current type redisplays cached text without another request");
    Access::toggleTranslation(window);
    require(translation->isChecked() &&
                window.persistenceSnapshot().textType == PinnedTextType::TranslationResults,
            "the explicit translation toolbar toggle synchronizes the menu preference");
    Access::toggleTranslation(window);
    require(original->isChecked(), "leaving translation through the toolbar selects recognition");
    translation->trigger();
    display->trigger();
    const auto record = window.persistenceSnapshot();
    require(!record.recognitionVisible && !record.translationVisible &&
                record.textType == PinnedTextType::TranslationResults,
            "hidden translation preference is independent of saved display visibility");

    config.restorePersistentState = true;
    config.persistedTextType = record.textType;
    config.persistedRecognitionResults = record.recognitionResults;
    config.recognitionResults = {};
    ScreenshotPinnedWindow restored;
    auto* restoredSession = Access::hiddenSelectionOffscreen(restored, config);
    auto* restoredTranslation =
        pinnedTextTypeAction(restored, "screenshotPinnedTranslationResultsAction");
    require(restoredTranslation->isChecked() && !restoredSession->active() &&
                pinnedTextTypeAction(restored, "screenshotPinnedTextTypeAction")->text() ==
                    QStringLiteral("Text type: Translation results"),
            "restoring hidden results preserves the selected type without activating it");
    restoredTranslation->trigger();
    require(restoredSession->originalImageTranslationActive() &&
                pinnedTextTypeAction(restored, "screenshotPinnedOcrAction")->isChecked() &&
                recognition.requests == 0,
            "reselecting restored translation displays it without OCR or translation requests");
    config.persistedRecognitionVisible = true;
    ScreenshotPinnedWindow visibleRestored;
    auto* visibleSession = Access::hiddenSelectionOffscreen(visibleRestored, config);
    Access::finishInitialTextDisplay(visibleRestored, config.persistedRecognitionVisible);
    require(visibleSession->originalImageTranslationActive() && recognition.requests == 0,
            "deferred presentation restores visible translated results from their saved type");

    class TextTypeTranslator final : public QTranslator {
      public:
        QString translate(const char* context, const char* source, const char*,
                          int) const override {
            if (QByteArray(context) == "ScreenshotPinnedWindow" &&
                (QByteArray(source) == "Text type: %1" || QByteArray(source) == "Show text" ||
                 QByteArray(source) == "Recognition results" ||
                 QByteArray(source) == "Translation results"))
                return QStringLiteral("Translated ") + QString::fromUtf8(source);
            return {};
        }
    } translator;
    QCoreApplication::installTranslator(&translator);
    const auto removeTranslator =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&window, &languageChange);
    require(type->text() ==
                    QStringLiteral("Translated Text type: Translated Translation results") &&
                display->text().section(QLatin1Char('\t'), 0, 0) ==
                    QStringLiteral("Translated Show text") &&
                original->text() == QStringLiteral("Translated Recognition results") &&
                translation->text() == QStringLiteral("Translated Translation results") &&
                translation->toolTip() == translation->text(),
            "existing text type menu labels and tooltips retranslate live");

    config.persistedTextType.reset();
    config.persistedTranslationVisible = true;
    ScreenshotPinnedWindow legacy;
    Access::hiddenSelectionOffscreen(legacy, config);
    require(legacy.persistenceSnapshot().textType == PinnedTextType::TranslationResults,
            "legacy translated visibility supplies the initial text type");
    config.persistedTextType = PinnedTextType::RecognitionResults;
    Access::hiddenSelectionOffscreen(legacy, config);
    require(legacy.persistenceSnapshot().textType == PinnedTextType::RecognitionResults,
            "an explicit saved selection overrides legacy visibility flags");
    ScreenshotPinnedWindow unavailable;
    require(!pinnedTextTypeAction(unavailable, "screenshotPinnedTextTypeAction")->isEnabled(),
            "text type is disabled together with unavailable recognition");
}

void pinnedTextTypeStartsTranslationAfterRecognition() {
    using Access = ScreenshotPinnedWindowTestAccess;
    using snow_shot::storage::PinnedTextType;
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto priorConfiguration = settings.configuration();
    const bool priorOverlay = settings.originalImageTranslationEnabled();
    const auto restoreSettings = qScopeGuard([&] {
        settings.setConfiguration(priorConfiguration);
        settings.setOriginalImageTranslationEnabled(priorOverlay);
    });
    require(settings.setConfiguration({QStringLiteral("en"), QStringLiteral("zh-Hans"),
                                       QStringLiteral("general"), QStringLiteral("original")}),
            "configure the local translation fixture");
    for (const bool overlay : {true, false}) {
        require(settings.setOriginalImageTranslationEnabled(overlay),
                "select translation presentation");
        translation_tests::Server server;
        SnowShotApiClient api(server.url());
        class DeferredRecognition final : public ScreenshotOcrRecognitionPort {
          public:
            Completion pending;
            int requests = 0;
            RequestToken recognize(ScreenshotOcrRequest request, QObject*,
                                   Completion completion) override {
                if (request.renderOnly)
                    return 0;
                ++requests;
                pending = std::move(completion);
                return 1;
            }
            void cancel(RequestToken) override {
                pending = {};
            }
            bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
                return false;
            }
        } recognition;
        auto config = cachedOcrPinConfig(&recognition);
        const auto result = *config.recognitionResults.text;
        config.recognitionResults = {};
        config.tableRecognition = &api;
        ScreenshotPinnedWindow window;
        auto* session = Access::hiddenSelectionOffscreen(window, config);
        {
            auto invalid = config;
            invalid.restorePersistentState = true;
            invalid.persistedRecognitionResults = QByteArrayLiteral("invalid saved recognition");
            invalid.persistedTextType = PinnedTextType::RecognitionResults;
            ScreenshotPinnedWindow invalidWindow;
            auto* invalidSession = Access::hiddenSelectionOffscreen(invalidWindow, invalid);
            Access::finishInitialTextDisplay(invalidWindow, true);
            require(!invalidSession->active() &&
                        !invalidWindow.persistenceSnapshot().recognitionVisible &&
                        recognition.requests == 0,
                    "invalid saved recognition keeps the default overlay hidden during restore");
        }
        auto* display = pinnedTextTypeAction(window, "screenshotPinnedOcrAction");
        auto* original = pinnedTextTypeAction(window, "screenshotPinnedRecognitionResultsAction");
        auto* translation =
            pinnedTextTypeAction(window, "screenshotPinnedTranslationResultsAction");
        translation->trigger();
        require(session->active() && display->isChecked() && recognition.requests == 1 &&
                    recognition.pending && server.streams.isEmpty(),
                "choosing translation activates text display and starts OCR before translation");
        original->trigger();
        auto completion = std::exchange(recognition.pending, {});
        completion(result);
        require(!session->translating() && server.streams.isEmpty(),
                "switching during OCR clears pending translation activation");
        translation->trigger();
        translation_tests::waitUntil([&] { return server.streams.size() == 1; },
                                     "start translation from recognized text");
        server.delta(0, QStringLiteral("Translated text"));
        translation_tests::waitUntil(
            [&] {
                return session->recognitionClipboardMimeData()->text() ==
                       QStringLiteral("Translated text");
            },
            "display streamed translation");
        require(window.persistenceSnapshot().recognitionVisible &&
                    window.persistenceSnapshot().translationVisible && translation->isChecked() &&
                    window.findChild<ScreenshotPinnedEditController*>() == nullptr,
                "both translation presentations persist visibility without opening a toolbar");
        original->trigger();
        server.delta(0, QStringLiteral(" late"));
        server.finish(0);
        translation_tests::waitUntil(
            [&] { return !session->workflowState().value(QStringLiteral("busy")).toBool(); },
            "finish the cached translation");
        require(!session->translating() && original->isChecked() &&
                    session->recognitionClipboardMimeData()->text() == QStringLiteral("Saved OCR"),
                "late translation updates cannot replace the selected recognition results");
        translation->trigger();
        require(session->translating() && server.streams.size() == 1 &&
                    session->recognitionClipboardMimeData()->text() ==
                        QStringLiteral("Translated text late"),
                "switching back reuses completed translation");

        if (!overlay) {
            const auto record = window.persistenceSnapshot();
            auto restoredConfig = config;
            restoredConfig.restorePersistentState = true;
            restoredConfig.persistedTextType = record.textType;
            restoredConfig.persistedRecognitionResults = record.recognitionResults;
            restoredConfig.persistedRecognitionVisible = record.recognitionVisible;
            restoredConfig.persistedTranslationVisible = record.translationVisible;
            ScreenshotPinnedWindow restored;
            auto* restoredSession = Access::hiddenSelectionOffscreen(restored, restoredConfig);
            Access::finishInitialTextDisplay(restored, restoredConfig.persistedRecognitionVisible);
            translation_tests::waitUntil(
                [&] { return server.streams.size() == 2; },
                "restore the visible translation editor using saved settings");
            server.delta(1, QStringLiteral("Restored translation"));
            server.finish(1);
            translation_tests::waitUntil(
                [&] {
                    return !restoredSession->workflowState().value(QStringLiteral("busy")).toBool();
                },
                "finish restored translation editor");
            require(restoredSession->editing() &&
                        restored.persistenceSnapshot().recognitionVisible &&
                        restoredSession->recognitionClipboardMimeData()->text() ==
                            QStringLiteral("Restored translation"),
                    "the translation editor restores its selection and display visibility "
                    "independently");
        }

        config.recognitionResults.text = result;
        config.recognitionResults.key = QStringLiteral("failure-fixture");
        ScreenshotPinnedWindow failed;
        auto* failedSession = Access::hiddenSelectionOffscreen(failed, config);
        const auto failedStream = server.streams.size();
        pinnedTextTypeAction(failed, "screenshotPinnedTranslationResultsAction")->trigger();
        translation_tests::waitUntil([&] { return server.streams.size() == failedStream + 1; },
                                     "start failing translation");
        server.fail(static_cast<int>(failedStream));
        translation_tests::waitUntil(
            [&] { return !failedSession->workflowState().value(QStringLiteral("busy")).toBool(); },
            "finish failed translation");
        require(failed.persistenceSnapshot().textType == PinnedTextType::TranslationResults &&
                    pinnedTextTypeAction(failed, "screenshotPinnedTranslationResultsAction")
                        ->isChecked(),
                "translation failure preserves the selected type");
        Access::toggleTextResults(failed);
        Access::toggleTextResults(failed);
        translation_tests::waitUntil([&] { return server.streams.size() == failedStream + 2; },
                                     "retry translation on redisplay");
        server.delta(static_cast<int>(failedStream + 1), QStringLiteral("Recovered translation"));
        server.finish(static_cast<int>(failedStream + 1));
        translation_tests::waitUntil(
            [&] { return !failedSession->workflowState().value(QStringLiteral("busy")).toBool(); },
            "complete translation retry");
        require(failedSession->recognitionClipboardMimeData()->text() ==
                    QStringLiteral("Recovered translation"),
                "translation results remain retryable after failure");
    }
}
void pinnedTranslatedTextKeepsWindowCommandsWithoutToolbar() {
    using Access = ScreenshotPinnedWindowTestAccess;
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto priorConfiguration = settings.configuration();
    const bool priorOverlay = settings.originalImageTranslationEnabled();
    const auto restoreSettings = qScopeGuard([&] {
        settings.setConfiguration(priorConfiguration);
        settings.setOriginalImageTranslationEnabled(priorOverlay);
    });
    require(settings.setConfiguration({QStringLiteral("en"), QStringLiteral("zh-Hans"),
                                       QStringLiteral("general"), QStringLiteral("original")}) &&
                settings.setOriginalImageTranslationEnabled(true),
            "configure formatted text translation");
    for (const bool html : {false, true}) {
        translation_tests::Server server;
        SnowShotApiClient api(server.url());
        auto config = cachedOcrPinConfig(nullptr);
        config.tableRecognition = &api;
        config.recognitionResults = {};
        config.formattedTextDocument = std::make_shared<QTextDocument>();
        if (html)
            config.formattedTextDocument->setHtml(QStringLiteral("<p><b>Source</b> text</p>"));
        else
            config.formattedTextDocument->setPlainText(QStringLiteral("Source text"));
        config.formattedPlainText = config.formattedTextDocument->toPlainText();
        ScreenshotPinnedWindow window;
        auto* session = Access::hiddenSelectionOffscreen(window, config);
        pinnedTextTypeAction(window, "screenshotPinnedTranslationResultsAction")->trigger();
        translation_tests::waitUntil([&] { return server.streams.size() == 1; },
                                     "start formatted text translation");
        server.delta(0, QStringLiteral("Translated text"));
        auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
        require(editor && editor->isVisible() && editor->isReadOnly() &&
                    window.findChild<ScreenshotPinnedEditController*>() == nullptr,
                "streamed text and HTML translation uses a read-only result without a toolbar");
        const auto openPinnedMenu = [&](bool hideResults) {
            bool inspected = false;
            QTimer::singleShot(0, &window, [&] {
                auto* menu = Access::contextMenu(window);
                require(menu && menu->isPopupVisible(),
                        "right-clicking translated text opens the pinned window context menu");
                if (hideResults) {
                    auto* display =
                        menu->findChild<QAction*>(QStringLiteral("screenshotPinnedOcrAction"));
                    require(display && display->isEnabled() && display->isChecked(),
                            "the result menu exposes the active text display toggle");
                    display->trigger();
                }
                menu->dismissPopup();
                inspected = true;
            });
            const QPoint position = editor->viewport()->rect().center();
            QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                                    editor->viewport()->mapToGlobal(position));
            QApplication::sendEvent(editor->viewport(), &event);
            QApplication::processEvents();
            require(inspected && event.isAccepted(), "the translation context menu was handled");
        };
        openPinnedMenu(false);
        Access::editSelectionOffscreen(window, true);
        require(editor->isReadOnly(), "opening the toolbar cannot edit an active stream");
        Access::editSelectionOffscreen(window, false);
        server.finish(0);
        translation_tests::waitUntil(
            [&] { return !session->workflowState().value(QStringLiteral("busy")).toBool(); },
            "finish formatted text translation");
        require(editor->isReadOnly() && editor->toPlainText() == QStringLiteral("Translated text"),
                "finished translation stays read-only while the toolbar is disabled");
        Access::editSelectionOffscreen(window, true);
        require(!editor->isReadOnly(), "the enabled toolbar permits editing completed translation");
        Access::editSelectionOffscreen(window, false);
        require(editor->isReadOnly(), "closing the toolbar restores read-only translation");
        window.activateWindow();
        editor->setFocus();
        waitForUi(30);
        require(editor->hasFocus(), "the translated result editor receives keyboard input");
        QKeyEvent typing(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QApplication::sendEvent(editor, &typing);
        require(editor->toPlainText() == QStringLiteral("Translated text"),
                "typing cannot alter toolbarless translation results");
        editor->selectAll();
        QApplication::clipboard()->clear();
        sendShortcut(*editor, Qt::Key_C, Qt::ControlModifier);
        require(QApplication::clipboard()->text() == QStringLiteral("Translated text"),
                "read-only translation still supports selecting and copying text");
        sendShortcut(*editor, Qt::Key_D, Qt::ControlModifier);
        require(!session->active(), "the window shortcut exits results from the focused editor");
        auto* canvas = window.findChild<SnowCanvasWidget*>();
        require(canvas != nullptr, "the pin retains its canvas after hiding results");
        canvas->setFocus();
        sendShortcut(*canvas, Qt::Key_D, Qt::ControlModifier);
        editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
        require(session->translating() && editor && editor->isReadOnly() &&
                    server.streams.size() == 1,
                "redisplaying cached translation preserves read-only interaction");
        openPinnedMenu(true);
        require(!session->active() && window.isVisible(),
                "the result context menu exits text results while keeping the pin open");
    }
}
} // namespace

#endif // SNOW_SHOT_TESTS_PINNED_TEXT_TYPE_TESTS_H
