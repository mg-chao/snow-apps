#ifndef SNOW_SHOT_TESTS_PINNED_TEXT_TYPE_TESTS_H
#define SNOW_SHOT_TESTS_PINNED_TEXT_TYPE_TESTS_H

#include "snow_shot/presentation/languagemanager.h"
#include <QCryptographicHash>

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

void pinnedTranslatedEditorPayloadIsBoundedAndOptional() {
    namespace persistence = snow_shot::presentation;
    auto source = cachedOcrPinConfig(nullptr).recognitionResults;
    source.translatedEditor = ScreenshotTranslatedTextEntry{
        source.key,
        QCryptographicHash::hash(QByteArrayLiteral("Saved OCR"), QCryptographicHash::Sha256),
        {QStringLiteral("en"), QStringLiteral("zh-Hans"), QStringLiteral("general"),
         QStringLiteral("original")},
        QStringLiteral("Translation baseline"),
        QStringLiteral("")};
    const QByteArray payload = persistence::encodeRecognitionModels(source);
    require(!payload.isEmpty() && persistence::canPersistTranslatedEditor(source),
            "a bounded translation with an empty draft is admissible");
    auto decoded = source;
    decoded.translatedEditor.reset();
    persistence::decodeRecognitionModels(payload, decoded);
    require(decoded.translatedEditor && decoded.translatedEditor->draft.has_value() &&
                decoded.translatedEditor->draft->isEmpty() &&
                decoded.translatedEditor->configuration == source.translatedEditor->configuration,
            "the optional editor payload retains all configuration fields and empty edits");
    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    for (int scenario = 0; scenario < 9; ++scenario) {
        auto malformed = root;
        auto entry = malformed.value(QStringLiteral("text_translation")).toObject();
        if (scenario == 0)
            entry.insert(QStringLiteral("version"), 2);
        else if (scenario == 1)
            entry.insert(QStringLiteral("source_key"), QStringLiteral("another-source"));
        else if (scenario == 2)
            entry.insert(QStringLiteral("source_fingerprint"), QStringLiteral("invalid"));
        else if (scenario == 3)
            entry.insert(QStringLiteral("baseline"), 12);
        else if (scenario == 4)
            entry.insert(QStringLiteral("draft"), QJsonValue(QJsonValue::Null));
        else {
            auto configuration = entry.value(QStringLiteral("configuration")).toObject();
            if (scenario == 5)
                configuration.remove(QStringLiteral("secondary_target_language"));
            else if (scenario == 6)
                configuration.insert(QStringLiteral("language_detection_confidence_threshold"),
                                     101);
            else if (scenario == 7)
                configuration.insert(QStringLiteral("language_detection_confidence_threshold"),
                                     60.5);
            else
                configuration.insert(QStringLiteral("layout_processing"),
                                     QStringLiteral("unsupported"));
            entry.insert(QStringLiteral("configuration"), configuration);
        }
        malformed.insert(QStringLiteral("text_translation"), entry);
        auto invalid = source;
        invalid.translatedEditor.reset();
        persistence::decodeRecognitionModels(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact), invalid);
        require(!invalid.translatedEditor && invalid.text && invalid.key == source.key,
                "invalid optional translated content cannot discard valid OCR results");
    }
    auto excessive = source;
    excessive.translatedEditor->draft = QString(4 * 1024 * 1024 + 1, u'x');
    require(!persistence::canPersistTranslatedEditor(excessive) &&
                persistence::encodeRecognitionModels(excessive).isEmpty(),
            "oversized user drafts are rejected explicitly instead of silently omitted");
    excessive.translatedEditor->draft = QString(3 * 1024 * 1024, QChar(1));
    require(!persistence::canPersistTranslatedEditor(excessive),
            "JSON escaping is included in the bounded translation payload estimate");
    QString projectedModel;
    QString projectedLanguage;
    ScreenshotRecognitionSessionActions actions;
    actions.setTextTranslationServices = [&](const QVector<SnowShotChatModel>& models,
                                             const QString& selection, bool) {
        require(models.isEmpty(),
                "retained provenance does not invent an available provider model");
        projectedModel = selection;
    };
    actions.setTextTargetLanguage = [&](const QString& language) { projectedLanguage = language; };
    ScreenshotRecognitionSessionController controller(nullptr, nullptr, nullptr,
                                                      std::move(actions));
    controller.setRetainCompletedTranslations(true);
    const auto config = cachedOcrPinConfig(nullptr);
    controller.setTarget(
        {source.key, config.imageSource.materializedImage, config.canvasSourceRect, {}, {}});
    auto wrongSource = source;
    wrongSource.translatedEditor->sourceFingerprint = QByteArray(32, 'x');
    controller.seedRecognitionResults(wrongSource);
    require(!controller.recognitionResultsSnapshot().translatedEditor && controller.hasTextResult(),
            "a translation bound to different source text cannot seed the current document");
    controller.seedRecognitionResults(source);
    controller.activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(projectedModel == source.translatedEditor->configuration.modelId &&
                projectedLanguage == source.translatedEditor->configuration.targetLanguage,
            "retained model and target controls reflect saved provenance without a provider");
    controller.setRetainCompletedTranslations(false);
    controller.synchronizeUiState();
    require(projectedModel.isEmpty(),
            "nonretained hosts continue to project their provider preferences");
    auto overlay = std::make_shared<ScreenshotOcrPresentation>();
    overlay->selection = source.text->presentation->selection;
    overlay->lines = source.text->presentation->lines;
    overlay->setLineText(0, QStringLiteral("Legacy overlay baseline"));
    overlay->prepareForRendering();
    wrongSource.translatedText = overlay;
    ScreenshotRecognitionSessionController independent(nullptr, nullptr, nullptr, {});
    independent.setIndependentEditing(true);
    independent.setTarget(
        {source.key, config.imageSource.materializedImage, config.canvasSourceRect, {}, {}});
    independent.seedRecognitionResults(wrongSource);
    const auto fallback = independent.recognitionResultsSnapshot();
    require(fallback.translatedEditor &&
                fallback.translatedEditor->baseline == QStringLiteral("Legacy overlay baseline"),
            "invalid optional editor binding preserves the independent host's legacy overlay seed");
}

void pinnedTranslatedEditorSurvivesRepositoryRestart() {
    using Access = ScreenshotPinnedWindowTestAccess;
    using snow_shot::storage::PinnedTextType;
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto priorConfiguration = settings.configuration();
    const bool priorOverlay = settings.originalImageTranslationEnabled();
    const auto restoreSettings = qScopeGuard([&] {
        settings.setConfiguration(priorConfiguration);
        settings.setOriginalImageTranslationEnabled(priorOverlay);
    });
    const snow_shot::storage::ScreenshotTranslationConfiguration savedConfiguration{
        QStringLiteral("en"),       QStringLiteral("zh-Hans"), QStringLiteral("general"),
        QStringLiteral("original"), QStringLiteral("fr"),      75};
    for (bool formatted : {false, true}) {
        for (bool emptyDraft : {false, true}) {
            for (bool visible : {false, true}) {
                require(settings.setConfiguration(savedConfiguration) &&
                            settings.setOriginalImageTranslationEnabled(false),
                        "configure saved translated editor");
                translation_tests::Server server;
                SnowShotApiClient api(server.url());
                auto config = cachedOcrPinConfig(nullptr);
                config.persistenceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                config.tableRecognition = &api;
                const QString original =
                    formatted ? QStringLiteral("Formatted source") : QStringLiteral("Saved OCR");
                if (formatted) {
                    config.formattedTextDocument = std::make_shared<QTextDocument>();
                    config.formattedTextDocument->setHtml(
                        QStringLiteral("<p><b>Formatted</b> source</p>"));
                    config.formattedPlainText = original;
                    config.originalClipboardContent.text = original;
                    config.originalClipboardContent.html = config.formattedTextDocument->toHtml();
                    config.recognitionResults.text.reset();
                }
                config.recognitionResults.translatedEditor = ScreenshotTranslatedTextEntry{
                    config.recognitionResults.key,
                    QCryptographicHash::hash(original.toUtf8(), QCryptographicHash::Sha256),
                    savedConfiguration, QStringLiteral("Retained baseline"), std::nullopt};
                ScreenshotPinnedWindow source;
                auto* session = Access::hiddenSelectionOffscreen(source, config);
                pinnedTextTypeAction(source, "screenshotPinnedTranslationResultsAction")->trigger();
                require(session->translating() &&
                            session->textDraft() == QStringLiteral("Retained baseline"),
                        "seed a completed translated document without requesting a provider");
                const QString draft = emptyDraft ? QStringLiteral("") : QStringLiteral("User edit");
                session->setTextDraft(draft);
                if (!visible)
                    Access::toggleTextResults(source);
                auto record = source.persistenceSnapshot();
                QTemporaryDir directory;
                require(directory.isValid(), "create translated editor repository");
                {
                    snow_shot::storage::PinnedWindowRepository repository(directory.path());
                    require(repository.upsert(record).success && repository.flush().success,
                            "persist the translated draft before retiring its source window");
                }
                source.close();
                auto changedConfiguration = savedConfiguration;
                changedConfiguration.targetLanguage = QStringLiteral("de");
                changedConfiguration.layoutProcessing = QStringLiteral("smart_merge");
                require(settings.setConfiguration(changedConfiguration) &&
                            settings.setOriginalImageTranslationEnabled(true),
                        "change global translation defaults before restoration");
                snow_shot::storage::PinnedWindowRepository reopened(directory.path());
                const auto loaded = reopened.loadRecord(record.id);
                require(loaded.has_value(),
                        "reload a translated editor after repository recreation");
                auto restoredConfig = config;
                restoredConfig.restorePersistentState = true;
                restoredConfig.recognitionResults = {};
                restoredConfig.persistedRecognitionResults = loaded->recognitionResults;
                restoredConfig.persistedTextType = loaded->textType;
                restoredConfig.persistedRecognitionVisible = loaded->recognitionVisible;
                restoredConfig.persistedTranslationVisible = loaded->translationVisible;
                // Exercise unavailable providers as well as the normal configured host.
                if (emptyDraft)
                    restoredConfig.tableRecognition = nullptr;
                ScreenshotPinnedWindow restored;
                auto* restoredSession = Access::hiddenSelectionOffscreen(restored, restoredConfig);
                Access::finishInitialTextDisplay(restored, loaded->recognitionVisible);
                if (!visible) {
                    require(!restoredSession->active() && restored.persistenceSnapshot().textType ==
                                                              PinnedTextType::TranslationResults,
                            "restoring hidden translated content retains only its selected type");
                    pinnedTextTypeAction(restored, "screenshotPinnedTranslationResultsAction")
                        ->trigger();
                }
                require(restoredSession->translating() && restoredSession->editing() &&
                            !restoredSession->originalImageTranslationActive() &&
                            restoredSession->textDraft() == draft && server.streams.isEmpty(),
                        "edited and empty translated documents survive restart without a request");
                auto subsequentConfiguration = changedConfiguration;
                subsequentConfiguration.layoutProcessing = QStringLiteral("original");
                require(settings.setConfiguration(subsequentConfiguration),
                        "change global layout while a retained editor is open");
                QApplication::processEvents();
                require(restoredSession->textDraft() == draft && server.streams.isEmpty(),
                        "global configuration notifications preserve the retained editor");
                if (!emptyDraft) {
                    auto& service = snow_shot::translation::TranslationService::forClient(
                        api, snow_shot::storage::ApplicationStorage::instance().configuration(),
                        snow_shot::presentation::LanguageManager::instance().currentLocale());
                    service.preferencesChanged(
                        snow_shot::translation::TranslationService::ChangeReason::UserPreferences);
                    service.modelInvalidated(savedConfiguration.modelId);
                    QApplication::processEvents();
                    require(restoredSession->textDraft() == draft && server.streams.isEmpty() &&
                                restoredSession->recognitionResultsSnapshot()
                                        .translatedEditor->baseline ==
                                    QStringLiteral("Retained baseline"),
                            "shared preference and model invalidation preserve retained user text");
                }
                restoredSession->resetTextEditing();
                require(restoredSession->textDraft() == QStringLiteral("Retained baseline") &&
                            server.streams.isEmpty(),
                        "Reset restores the saved successful baseline without a provider");
                const auto snapshot = restoredSession->recognitionResultsSnapshot();
                require(snapshot.translatedEditor &&
                            snapshot.translatedEditor->configuration == savedConfiguration &&
                            !snapshot.translatedEditor->draft.has_value(),
                        "restored baseline keeps its original configuration provenance");
                if (formatted && !emptyDraft && visible) {
                    ScreenshotPinnedWindow sibling;
                    auto* siblingSession =
                        Access::hiddenSelectionOffscreen(sibling, restoredConfig);
                    pinnedTextTypeAction(sibling, "screenshotPinnedTranslationResultsAction")
                        ->trigger();
                    restoredSession->applyTextTargetLanguage(
                        subsequentConfiguration.targetLanguage);
                    translation_tests::waitUntil([&] { return server.streams.size() == 1; },
                                                 "explicit local settings start replacement work");
                    require(siblingSession->textDraft() == draft &&
                                siblingSession->recognitionResultsSnapshot()
                                        .translatedEditor->configuration == savedConfiguration,
                            "local replacement applies global defaults only to the requesting pin");
                    server.delta(0, QStringLiteral("New local translation"));
                    server.finish(0);
                    translation_tests::waitUntil(
                        [&] {
                            return !restoredSession->workflowState()
                                        .value(QStringLiteral("busy"))
                                        .toBool();
                        },
                        "finish the explicitly requested replacement");
                }
            }
        }
    }
}

void pinnedTextTypeStartsTranslationAfterRecognition() {
    using Access = ScreenshotPinnedWindowTestAccess;
    using snow_shot::storage::PinnedTextType;
    pinnedTranslatedEditorPayloadIsBoundedAndOptional();
    pinnedTranslatedEditorSurvivesRepositoryRestart();
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
            require(restoredSession->editing() &&
                        restored.persistenceSnapshot().recognitionVisible &&
                        server.streams.size() == 1 &&
                        restoredSession->recognitionClipboardMimeData()->text() ==
                            QStringLiteral("Translated text late"),
                    "restore completed translated text without a replacement provider request");
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
