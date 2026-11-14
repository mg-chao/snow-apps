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
#include "recognition_attachment_test_access.h"
#include <QKeyEvent>
#include <QKeySequence>
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
using snow_shot::presentation::ImageEditingDocument;
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

class ImmediateRecognition final : public ScreenshotOcrRecognitionPort {
  public:
    RequestToken recognize(ScreenshotOcrRequest request, QObject*, Completion completion) override {
        auto result = recognitionFixture().text.value();
        result.presentation->selection = request.canvasRect.toRect();
        result.presentation->lines[0].text = QStringLiteral("Cropped recognition");
        completion(std::move(result));
        return ++token;
    }
    void cancel(RequestToken) override {}
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }
    RequestToken token = 0;
};

void cropSessionCopyUsesAttachedController() {
    ImmediateRecognition provider;
    ImageEditingWindow editor(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    ImageEditingSource source;
    source.image = QImage(100, 60, QImage::Format_ARGB32_Premultiplied);
    source.image.fill(Qt::white);
    source.canvasRect = QRectF(0, 0, 100, 60);
    source.recognition = recognitionFixture();
    editor.present(source);
    editor.palette()->textTranslationRequested();
    const QPointer<Session> original = editor.recognition();
    original->setTextDraft(QStringLiteral("Edited A translation"));
    editor.palette()->moveRequested();
    require(editor.document()->crop(ImageEditingDocument::CropAxis::Rows, 30, 35),
            "copy fixture creates crop B");
    require(!original->hasTarget(), "inactive translation releases target pixels");
    editor.palette()->ocrRequested();
    const QPointer<Session> cropped = editor.recognition();
    require(cropped && cropped != original && cropped->hasTextResult(),
            "crop B gets independent deterministic recognition");
    editor.palette()->textEditRequested();
    cropped->setTextDraft(QStringLiteral("B draft"));
    editor.palette()->moveRequested();
    require(editor.document()->undo(), "copy fixture undoes crop B");
    editor.palette()->textTranslationRequested();
    require(editor.recognition() == original && original->translating() &&
                original->textDraft() == QStringLiteral("Edited A translation"),
            "undo restores A's edited translation and active controller");
    auto* view = editor.canvas()->parentWidget()->findChild<ScreenshotRecognitionWindow*>(
        QStringLiteral("imageEditingRecognition"));
    require(view && original->ownsContentAttachment(), "A owns the shared view after undo");
    const auto viewport = [&] {
        return editor.canvas()->canvasToViewTransform().inverted().mapRect(
            QRectF(editor.canvas()->rect()));
    };
    require(ScreenshotRecognitionWindowTestAccess::canvasSelection(*view) == viewport(),
            "cached activation presents the restored crop geometry");
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const bool previousCopy = settings.copyTranslationResultsAsImage();
    const auto restore = qScopeGuard(
        [&] { static_cast<void>(settings.setCopyTranslationResultsAsImage(previousCopy)); });
    for (const bool imageCopy : {true, false}) {
        require(settings.setCopyTranslationResultsAsImage(imageCopy),
                "select Copy output preference");
        const auto assertClipboard = [&] {
            const auto* mime = QApplication::clipboard()->mimeData();
            require(mime && mime->hasImage() == imageCopy && mime->hasText() != imageCopy &&
                        (imageCopy || mime->text() == QStringLiteral("Edited A translation")),
                    "all unselected Copy routes follow A's translation policy");
        };
        {
            adqt::widgets::AdContextMenu menu(view);
            ScreenshotRecognitionWindowTestAccess::populateTextMenu(*view, menu);
            auto* copy =
                menu.findChild<QAction*>(QStringLiteral("screenshotRecognitionCopyAction"));
            require(copy && copy->isEnabled(), "restored translation context Copy is enabled");
            copy->trigger();
            assertClipboard();
        }
        require(view->copyVisibleContentToClipboard(), "restored direct-view Copy succeeds");
        assertClipboard();
        editor.palette()->copyRequested();
        assertClipboard();
        auto* text = view->findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
        require(text, "restored translation owns a text editor");
        text->setFocus();
        QApplication::processEvents();
        bool sawPublication = false;
        const auto observe =
            QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, view, [&] {
                sawPublication = true;
                assertClipboard();
            });
        QKeyEvent key(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, 0x2e, 0x43, 0,
                      QStringLiteral("c"));
        QApplication::sendEvent(text, &key);
        QObject::disconnect(observe);
        require(sawPublication, "keyboard Copy publishes the correct MIME from its first write");
        auto cursor = text->textCursor();
        cursor.setPosition(0);
        cursor.setPosition(6, QTextCursor::KeepAnchor);
        text->setTextCursor(cursor);
        require(view->copyVisibleContentToClipboard() &&
                    QApplication::clipboard()->text() == QStringLiteral("Edited") &&
                    !QApplication::clipboard()->mimeData()->hasImage(),
                "selected translation text precedes image preference");
        {
            adqt::widgets::AdContextMenu menu(view);
            ScreenshotRecognitionWindowTestAccess::populateTextMenu(*view, menu);
            menu.findChild<QAction*>(QStringLiteral("screenshotRecognitionCopyAction"))->trigger();
            require(QApplication::clipboard()->text() == QStringLiteral("Edited") &&
                        !QApplication::clipboard()->mimeData()->hasImage(),
                    "context Copy preserves selected translation text");
        }
        editor.palette()->copyRequested();
        require(QApplication::clipboard()->text() == QStringLiteral("Edited") &&
                    !QApplication::clipboard()->mimeData()->hasImage(),
                "toolbar Copy preserves selected translation text");
        cursor.clearSelection();
        text->setTextCursor(cursor);
    }
    editor.palette()->moveRequested();
    require(editor.document()->redo(), "copy fixture redoes B");
    editor.palette()->ocrRequested();
    editor.palette()->textEditRequested();
    require(editor.recognition() == cropped && cropped->textDraft() == QStringLiteral("B draft") &&
                ScreenshotRecognitionWindowTestAccess::canvasSelection(*view) == viewport() &&
                view->copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("B draft"),
            "redo restores B's recognition draft and current Copy policy");
    editor.palette()->moveRequested();
    require(editor.document()->undo(), "copy fixture returns to A before branching");
    require(editor.document()->crop(ImageEditingDocument::CropAxis::Columns, 40, 45),
            "new crop discards B's redo branch");
    editor.palette()->ocrRequested();
    const QPointer<Session> current = editor.recognition();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!cropped && current && current->ownsContentAttachment() &&
                view->copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("Cropped recognition"),
            "deferred destruction of pruned B preserves the new crop's attachment");
    editor.shutdown();
}

void editCachedTranslation(const ImageEditingSource& source) {
    ImageEditingWindow editor;
    editor.present(source);
    editor.palette()->textTranslationRequested();
    auto* session = editor.recognition();
    QPointer<QTextEdit> text = editor.canvas()->parentWidget()->findChild<QTextEdit*>();
    require(session && session->translating() && session->editing() &&
                !session->originalImageTranslationActive(),
            "cached translation respects the editable text preference");
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

void copyCachedTranslationOverlay(const ImageEditingSource& source) {
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const bool previousCopy = settings.copyTranslationResultsAsImage();
    const auto restore = qScopeGuard(
        [&] { static_cast<void>(settings.setCopyTranslationResultsAsImage(previousCopy)); });
    require(settings.setCopyTranslationResultsAsImage(true), "select translated image copy");
    ImageEditingWindow editor;
    editor.present(source);
    editor.palette()->textTranslationRequested();
    auto* session = editor.recognition();
    auto* view = editor.canvas()->findChild<ScreenshotRecognitionWindow*>();
    require(session && view && view->boxSelectionEnabled() &&
                session->originalImageTranslationActive() && !editor.dirty(),
            "cached translation is selectable on the image and preserves the import checkpoint");
    require(session->findChildren<snow_shot::translation::TranslationJob*>().isEmpty(),
            "cached overlay avoids a new translation request");
    editor.canvas()->actualSize();
    editor.palette()->copyRequested();
    const auto* mime = QApplication::clipboard()->mimeData();
    const auto image = qvariant_cast<QImage>(mime->imageData());
    require(mime->hasImage() && !mime->hasText() && image.size() == source.image.size() &&
                image.pixelColor(0, 0) == source.image.pixelColor(0, 0) && image != source.image,
            "translation image copy composites text over the base image at source resolution");
    view->setFocus();
    QApplication::processEvents();
    auto* presentation = ScreenshotRecognitionWindowTestAccess::ocrPresentation(*view);
    require(presentation, "the translated OCR presentation belongs to the shared view");
    presentation->selectAll();
    view->updateOcrSelection();
    adqt::widgets::AdContextMenu menu(view);
    ScreenshotRecognitionWindowTestAccess::populateOcrMenu(*view, menu);
    auto* copy = menu.findChild<QAction*>(QStringLiteral("screenshotOcrCopyAction"));
    require(copy && copy->isEnabled(), "the translated overlay has a context Copy action");
    copy->trigger();
    require(QApplication::clipboard()->text() == QStringLiteral("Translated text") &&
                !QApplication::clipboard()->mimeData()->hasImage(),
            "selected translated text takes precedence over image copy");
    editor.palette()->copyRequested();
    require(QApplication::clipboard()->text() == QStringLiteral("Translated text") &&
                !editor.dirty(),
            "toolbar copy uses the same selection and preserves the checkpoint");
    QApplication::clipboard()->setText(QStringLiteral("keyboard copy sentinel"));
    const auto key = QKeySequence::keyBindings(QKeySequence::Copy).first()[0];
    for (const auto type : {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
        QKeyEvent event(type, int(key.key()), key.keyboardModifiers());
        QApplication::sendEvent(view, &event);
    }
    require(QApplication::clipboard()->text() == QStringLiteral("Translated text"),
            "keyboard copy uses the same translated selection");
    editor.shutdown();
}

void quickServicePickerDiscoversModelsWithoutATranslationJob(bool imageMode) {
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
                    session->originalImageTranslationActive() == imageMode &&
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
        server.delta(0, QStringLiteral("Server"));
        auto* view = editor.canvas()->findChild<ScreenshotRecognitionWindow*>();
        awaitCondition(
            [&] {
                if (session->textDraft() != QStringLiteral("Server"))
                    return false;
                const auto* presentation =
                    view && imageMode
                        ? ScreenshotRecognitionWindowTestAccess::ocrPresentation(*view)
                        : nullptr;
                return !imageMode ||
                       (presentation && presentation->lines[0].text == QStringLiteral("Server"));
            },
            "the selected service displays an incremental translation");
        require(view && view->showsOcrLayer() == imageMode,
                "streaming updates the selected recognition page");
        if (imageMode) {
            auto* presentation = ScreenshotRecognitionWindowTestAccess::ocrPresentation(*view);
            require(presentation && presentation->lines[0].text == QStringLiteral("Server") &&
                        presentation->lines[0].translationStreaming,
                    "the shared OCR layer displays incremental translated text");
        }
        if (imageMode && rejectFirst) {
            ImageEditingSource replacement;
            replacement.image = QImage(60, 40, QImage::Format_ARGB32_Premultiplied);
            replacement.image.fill(Qt::green);
            replacement.canvasRect = QRectF(0, 0, 60, 40);
            server.delta(0, QStringLiteral(" late result"));
            server.finish(0);
            editor.present(std::move(replacement));
            QApplication::processEvents();
            require(!editor.recognition() && view->isHidden() &&
                        editor.document()->image().size() == QSize(60, 40),
                    "a replaced document retires the streaming overlay");
            editor.shutdown();
            require(settings.setConfiguration(configuration), "restore the free service");
            continue;
        }
        server.delta(0, QStringLiteral(" translation"));
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
    require(settings.setOriginalImageTranslationEnabled(false), "select editable translation");
    ImageEditingSource cached;
    cached.image = QImage(100, 60, QImage::Format_ARGB32_Premultiplied);
    cached.image.fill(Qt::white);
    cached.canvasRect = QRectF(0, 0, 100, 60);
    cached.recognition = recognitionFixture();
    cropSessionCopyUsesAttachedController();
    editCachedTranslation(cached);
    quickServicePickerDiscoversModelsWithoutATranslationJob(false);
    independentModeAfterSeedingKeepsCapturedText();
    require(settings.setOriginalImageTranslationEnabled(true), "select on-image translation");
    copyCachedTranslationOverlay(cached);
    quickServicePickerDiscoversModelsWithoutATranslationJob(true);
    liveAndPersistedTranslationImports();
    ordinaryHostsKeepCapturedOverlay();
#endif
}
