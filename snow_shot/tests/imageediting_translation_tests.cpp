// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotpinnedtextpreparation.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/translation/freetranslation.h"
#include "translation_test_support.h"
#include "widgets/select.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPointer>
#include <QScopeGuard>
#include <QScreen>
#include <QTextEdit>
#include <QThread>
#include <stdexcept>

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
namespace {
using snow_shot::presentation::ImageEditingSource;
using snow_shot::presentation::ImageEditingWindow;
using Session = ScreenshotRecognitionSessionController;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void awaitCondition(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

ScreenshotRecognitionResults recognitionFixture() {
    auto original = std::make_shared<ScreenshotOcrPresentation>();
    original->selection = QRect(0, 0, 100, 60);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Original text");
    line.quad = {QPointF(5, 5), QPointF(95, 5), QPointF(95, 25), QPointF(5, 25)};
    original->lines.append(line);
    original->prepareForRendering();
    auto translated = std::make_shared<ScreenshotOcrPresentation>();
    translated->selection = original->selection;
    translated->lines = original->lines;
    translated->lines[0].text = QStringLiteral("Translated text");
    translated->prepareForRendering();
    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("image-editing-translation");
    results.text = ScreenshotOcrRecognitionResult{original};
    results.translatedText = std::move(translated);
    return results;
}

void editCachedTranslation(const ImageEditingSource& source) {
    ImageEditingWindow editor;
    editor.present(source);
    editor.palette()->textTranslationRequested();
    auto* session = editor.recognition();
    QPointer<QTextEdit> text = editor.canvas()->parentWidget()->findChild<QTextEdit*>();
    require(session && session->translating() && session->editing() &&
                !session->originalImageTranslationActive(),
            "independent cached translation uses editable text instead of the image overlay");
    require(text && text->isVisible() && !text->isReadOnly() &&
                text->toPlainText() == QStringLiteral("Translated text") &&
                session->textDraft() == text->toPlainText(),
            "cached translation displays the same text that export reads without a provider");
    require(session->findChildren<snow_shot::translation::TranslationJob*>().isEmpty(),
            "restoring a complete captured translation does not start another translation job");
    require(!editor.dirty(), "viewing a captured translation preserves the import checkpoint");
    session->setTextDraft(QStringLiteral("Edited translation"));
    require(text->toPlainText() == QStringLiteral("Edited translation"),
            "the captured translation document accepts independent edits");
    editor.palette()->copyRequested();
    require(QApplication::clipboard()->text() == text->toPlainText(),
            "Copy publishes the translation currently displayed in its editor");
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto restoreCopy =
        qScopeGuard([&] { static_cast<void>(settings.setCopyTranslationResultsAsImage(false)); });
    require(settings.setCopyTranslationResultsAsImage(true),
            "enable editor translation image copy");
    editor.palette()->copyRequested();
    require(QApplication::clipboard()->mimeData()->hasImage() &&
                !QApplication::clipboard()->mimeData()->hasText(),
            "editor Copy publishes the translated document as an image");
    auto cursor = text->textCursor();
    cursor.setPosition(0);
    cursor.setPosition(6, QTextCursor::KeepAnchor);
    text->setTextCursor(cursor);
    editor.palette()->copyRequested();
    require(QApplication::clipboard()->text() == QStringLiteral("Edited") &&
                !QApplication::clipboard()->mimeData()->hasImage(),
            "editor Copy preserves selected translation text");
    require(settings.setCopyTranslationResultsAsImage(false), "restore editor copy preference");
    editor.palette()->ocrRequested();
    require(!text, "switching recognition tools retires the preceding text editor widget");
    editor.palette()->textTranslationRequested();
    text = editor.canvas()->parentWidget()->findChild<QTextEdit*>();
    require(text && text->isVisible() && session->translating() &&
                !session->originalImageTranslationActive() &&
                text->toPlainText() == QStringLiteral("Edited translation"),
            "returning to cached translation preserves the edited translation document");
    session->resetTextEditing();
    require(text->toPlainText() == QStringLiteral("Translated text"),
            "Reset restores the captured translation as the editing baseline");
    editor.shutdown();
}

void liveAndPersistedTranslationImports() {
    QImage image(100, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    require(config.screen, "translation import fixture needs an offscreen screen");
    config.canvasSourceRect = QRectF(0, 0, 100, 60);
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.initialWindowSize = image.size();
    config.nativeGeometry = QRect(QPoint(40, 40), image.size());
    config.automaticTextRecognition = false;
    config.recognitionResults = recognitionFixture();
    ScreenshotPinnedWindow pin;
    pin.setAttribute(Qt::WA_DeleteOnClose, false);
    bool published = false;
    require(pin.present(config, [&](bool success, QImage) { published = success; }),
            "translation source pin presents");
    awaitCondition([&] { return published; }, "translation source pin publishes its image");
    ImageEditingSource cached;
    cached.image = image;
    cached.canvasRect = config.canvasSourceRect;
    cached.recognition = recognitionFixture();
    editCachedTranslation(cached);
    auto* session = pin.findChild<Session*>();
    awaitCondition([&] { return session && session->hasTextResult(); },
                   "the pinned source seeds its cached text presentation");
    session->activate(Session::Mode::Text);
    session->beginTextTranslation();
    require(session->originalImageTranslationActive(),
            "the source pin displays its cached translated image content");
    ImageEditingWindow live;
    bool loaded = false;
    QObject::connect(&live, &ImageEditingWindow::loaded, &live, [&](bool success) {
        require(success, "live translated raster loads into independent editing");
        loaded = true;
    });
    pin.requestRenderedImageSnapshot([&](ScreenshotPinnedImageSnapshot snapshot) {
        require(snapshot.error.isEmpty(), "translated pin prepares its shared rendered source");
        live.presentSource(std::move(snapshot.source));
    });
    awaitCondition([&] { return loaded; }, "the live translated render completes");
    const auto persisted = snow_shot::presentation::readImageEditingPinned(
        pin.persistenceSnapshot(), captureScreenshotPinnedRenderPreparation());
    require(persisted.error.isEmpty() && persisted.recognition.isEmpty() &&
                !live.document()->runtime().hasDocumentContent() &&
                persisted.image == live.document()->image() && persisted.image != image,
            "live and persisted translated pins import the same rendered image without stale OCR");
    live.shutdown();
    pin.hide();
}

void independentFreshTranslationKeepsTextPresentation() {
    auto cached = recognitionFixture();
    cached.translatedText.reset();
    QImage image(100, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    Session session(nullptr, nullptr, nullptr, {});
    session.setIndependentEditing(true);
    session.setTarget({cached.key, image, QRectF(0, 0, 100, 60)});
    session.seedRecognitionResults(cached);
    session.activate(Session::Mode::Text);
    session.beginTextTranslation();
    require(session.translating() && session.editing() &&
                !session.originalImageTranslationActive() && session.textDraft().isEmpty(),
            "a fresh independent translation keeps text editing even with image mode enabled");
}

void quickServicePickerDiscoversModelsWithoutATranslationJob() {
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto original = settings.configuration();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setConfiguration(original)); });
    auto configuration = original;
    configuration.modelId = snow_shot::translation::freeTranslationId;
    require(settings.setConfiguration(configuration), "select the default free service");
    for (const bool rejectFirst : {false, true}) {
        translation_tests::Server server;
        server.holdModels = true;
        server.rejectModels = rejectFirst;
        SnowShotApiClient api(server.url());
        ImageEditingWindow editor(
            {[&] { return ScreenshotPinnedRecognitionProviders{nullptr, nullptr, &api}; }});
        ImageEditingSource source;
        source.image = QImage(100, 60, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(Qt::white);
        source.canvasRect = QRectF(0, 0, 100, 60);
        source.recognition = recognitionFixture();
        editor.present(std::move(source));
        editor.palette()->textTranslationRequested();
        QApplication::processEvents();
        auto* session = editor.recognition();
        auto* select = editor.palette()->findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("screenshotTranslationQuickServiceSelect"));
        auto& service = snow_shot::translation::TranslationService::forClient(
            api, snow_shot::storage::ApplicationStorage::instance().configuration(),
            QLocale::English);
        require(session && select && session->translating() && select->options().size() == 1 &&
                    select->currentValue() == configuration.modelId && server.modelRequests == 0 &&
                    session->findChildren<snow_shot::translation::TranslationJob*>().isEmpty(),
                "a fresh free-service picker starts with usable local options and cached text");
        select->setPopupVisible(true);
        awaitCondition([&] { return server.modelRequests == 1; },
                       "opening the quick picker discovers built-in services without a job");
        require(select->popupVisible() && select->isEnabled() && service.loadingModels() &&
                    select->currentValue() == configuration.modelId,
                "local services stay usable while catalog discovery is pending");
        select->setPopupVisible(false);
        select->setPopupVisible(true);
        QApplication::processEvents();
        require(server.modelRequests == 1, "reopening shares the pending catalog request");
        select->setSearchText(QStringLiteral("AI"));
        server.respondModels();
        awaitCondition([&] { return !service.loadingModels(); }, "catalog discovery settles");
        if (rejectFirst) {
            require(select->isEnabled() && select->options().size() == 1 &&
                        select->currentValue() == configuration.modelId &&
                        service.errorText().isEmpty(),
                    "catalog failure preserves the selected local service");
            select->setPopupVisible(false);
            select->setPopupVisible(true);
            awaitCondition([&] { return server.modelRequests == 2; },
                           "reopening retries a failed catalog request");
            select->setSearchText(QStringLiteral("AI"));
            server.rejectModels = false;
            server.respondModels();
            awaitCondition([&] { return !service.loadingModels(); }, "catalog retry settles");
        }
        require(select->popupVisible() && select->options().size() == 4 &&
                    select->searchText() == QStringLiteral("AI") &&
                    select->currentValue() == configuration.modelId &&
                    session->textDraft() == QStringLiteral("Translated text") &&
                    session->findChildren<snow_shot::translation::TranslationJob*>().isEmpty() &&
                    server.streams.isEmpty(),
                "discovery preserves the open picker's search, selection, and cached text");
        select->setPopupVisible(false);
        select->setPopupVisible(true);
        awaitCondition([&] { return !service.loadingModels(); }, "cached catalog is republished");
        require(server.modelRequests == (rejectFirst ? 2 : 1),
                "reopening a loaded picker reuses the catalog cache");
        select->setPopupVisible(false);
        select->setCurrentValue(QStringLiteral("general"));
        awaitCondition([&] { return server.streams.size() == 1; },
                       "the newly discovered service translates the original text");
        const auto body = server.streams.first().body;
        const auto content = body.value(QStringLiteral("messages")).toArray().last().toObject();
        require(body.value(QStringLiteral("model")) == QStringLiteral("general") &&
                    content.value(QStringLiteral("content")) == QStringLiteral("Original text"),
                "switching services sends the original source to the selected model");
        server.delta(0, QStringLiteral("Server translation"));
        server.finish(0);
        awaitCondition([&] { return session->textDraft() == QStringLiteral("Server translation"); },
                       "the selected service publishes its translation");
        editor.shutdown();
        require(settings.setConfiguration(configuration),
                "restore the free service for the next host");
    }
}

void independentModeAfterSeedingKeepsCapturedText() {
    const auto cached = recognitionFixture();
    QImage image(100, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    Session session(nullptr, nullptr, nullptr, {});
    session.setTarget({cached.key, image, QRectF(0, 0, 100, 60)});
    session.seedRecognitionResults(cached);
    session.setIndependentEditing(true);
    session.activate(Session::Mode::Text);
    require(!session.activateCachedTextTranslation(),
            "cached overlay activation cannot bypass independent presentation mode");
    session.beginTextTranslation();
    require(session.editing() && !session.originalImageTranslationActive() &&
                session.textDraft() == QStringLiteral("Translated text"),
            "enabling independent editing after seeding retains the captured translation text");
}

void ordinaryHostsKeepCapturedOverlay() {
    const auto cached = recognitionFixture();
    QImage image(100, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    Session session(nullptr, nullptr, nullptr, {});
    session.setTarget({cached.key, image, QRectF(0, 0, 100, 60)});
    session.seedRecognitionResults(cached);
    session.activate(Session::Mode::Text);
    session.beginTextTranslation();
    const auto mime = session.recognitionClipboardMimeData();
    require(session.originalImageTranslationActive() && mime &&
                mime->text() == QStringLiteral("Translated text"),
            "ordinary recognition hosts continue displaying and exporting the captured overlay");
}
} // namespace
#endif

void runImageEditingTranslationTests() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const bool originalImage = settings.originalImageTranslationEnabled();
    const auto restore = qScopeGuard(
        [&] { static_cast<void>(settings.setOriginalImageTranslationEnabled(originalImage)); });
    require(settings.setOriginalImageTranslationEnabled(true),
            "translation regression selects image translation for ordinary hosts");
    liveAndPersistedTranslationImports();
    quickServicePickerDiscoversModelsWithoutATranslationJob();
    independentModeAfterSeedingKeepsCapturedText();
    independentFreshTranslationKeepsTextPresentation();
    ordinaryHostsKeepCapturedOverlay();
#endif
}
