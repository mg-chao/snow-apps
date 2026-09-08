#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"

#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/switch.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>

#include <iostream>
#include <memory>
#include <utility>

void runOriginalImageTranslationTests();
void runImageConversionTests();

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void processFor(int durationMs) {
    QEventLoop loop;
    QTimer::singleShot(durationMs, &loop, &QEventLoop::quit);
    loop.exec();
}

// Recognition port stand-in whose asset readiness and download phase are
// steered by the test, mirroring ScreenshotOcrAssets status reporting.
class ControllableOcrRecognition final : public ScreenshotOcrRecognitionPort {
  public:
    RequestToken recognize(ScreenshotOcrRequest, QObject*, Completion completion) override {
        ++requests;
        m_completion = std::move(completion);
        return 1;
    }

    void cancel(RequestToken) override {}

    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }

    bool modelFilesReady() const override {
        return m_ready;
    }

    ScreenshotOcrAssetStatus assetStatus() const override {
        return m_status;
    }

    void completeWithEmptyPresentation() {
        ScreenshotOcrRecognitionResult result;
        result.presentation = std::make_shared<ScreenshotOcrPresentation>();
        result.presentation->prepareForRendering();
        if (m_completion) {
            m_completion(std::move(result));
        }
    }

    int requests = 0;
    bool m_ready = false;
    ScreenshotOcrAssetStatus m_status{ScreenshotOcrAssetPhase::Verifying, QStringLiteral("assets")};

  private:
    Completion m_completion;
};

struct PromptRecorder {
    int modelDownloadShows = 0;
    int modelDownloadHides = 0;
    int recognitionShows = 0;
    QStringList modelDownloadMessages;

    ScreenshotRecognitionSessionActions actions() {
        ScreenshotRecognitionSessionActions result;
        result.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
        result.showModelDownload = [this](const QString& message) {
            ++modelDownloadShows;
            modelDownloadMessages.push_back(message);
        };
        result.hideModelDownload = [this]() { ++modelDownloadHides; };
        result.showRecognition = [this](const QString&) { ++recognitionShows; };
        return result;
    }
};

std::unique_ptr<ScreenshotRecognitionSessionController>
makeTextSession(ControllableOcrRecognition& recognition, PromptRecorder& recorder) {
    auto controller = std::make_unique<ScreenshotRecognitionSessionController>(
        &recognition, nullptr, nullptr, recorder.actions());
    ScreenshotRecognitionTarget target;
    target.key = QStringLiteral("session");
    target.image = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    target.canvasRect = QRectF(QPointF(), QSizeF(target.image.size()));
    controller->setTarget(target);
    return controller;
}

// A cached launch pays asset re-verification and helper start-up before the
// first recognition can run; none of that may surface the download prompt.
void cachedVerificationStaysSilent() {
    ControllableOcrRecognition recognition;
    PromptRecorder recorder;
    auto controller = makeTextSession(recognition, recorder);
    controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(recognition.requests == 1, "activating text mode should queue one request");
    require(recorder.modelDownloadShows == 0,
            "asset verification alone must not surface the download prompt");
    require(recorder.recognitionShows == 1,
            "active text recognition should show the plain recognition message");

    processFor(350);
    require(recorder.modelDownloadShows == 0 && recorder.modelDownloadHides == 0,
            "cache verification must never touch the download prompt");

    recognition.m_ready = true;
    recognition.m_status = {ScreenshotOcrAssetPhase::ReadyCached, QStringLiteral("assets")};
    recognition.completeWithEmptyPresentation();
    processFor(250);
    require(recorder.modelDownloadShows == 0 && recorder.modelDownloadHides == 0,
            "a cached launch must never touch the download prompt");
    require(recognition.requests == 1, "a cached launch must not requeue recognition");
}

void cachedRecognitionUsesTheSelectedFillStyle() {
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    require(configuration.setValue(QStringLiteral("text_recognition/fill_style"),
                                   QStringLiteral("background_fill")),
            "select background fill");
    ControllableOcrRecognition recognition;
    std::shared_ptr<ScreenshotOcrPresentation> displayed;
    ScreenshotRecognitionSessionActions actions;
    actions.applyOcrPresentation = [&](const auto& presentation) { displayed = presentation; };
    ScreenshotRecognitionSessionController controller(&recognition, nullptr, nullptr, actions);
    ScreenshotRecognitionTarget target;
    target.key = QStringLiteral("fill-session");
    target.image = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    target.image.fill(QColor(30, 40, 50));
    target.canvasRect = QRectF(0, 0, 64, 64);
    controller.setTarget(target);
    ScreenshotRecognitionResults cached;
    cached.key = target.key;
    cached.text = ScreenshotOcrRecognitionResult{};
    cached.text->presentation = std::make_shared<ScreenshotOcrPresentation>();
    cached.text->presentation->selection = QRect(0, 0, 64, 64);
    cached.text->presentation->lines.push_back(
        {QStringLiteral("Text"), 1.0,
         QPolygonF{QPointF(10, 10), QPointF(50, 10), QPointF(50, 30), QPointF(10, 30)}});
    cached.text->presentation->prepareForRendering();
    controller.seedRecognitionResults(cached);
    controller.activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(displayed != nullptr && displayed->lines[0].backgroundFillColor == QColor(30, 40, 50),
            "cached OCR must receive fill colors from the source image before display");
    require(configuration.setValue(QStringLiteral("text_recognition/fill_style"),
                                   QStringLiteral("blur")),
            "restore Blur");
    require(displayed != nullptr && !displayed->lines[0].backgroundFillColor.isValid() &&
                recognition.requests == 0,
            "switching visible OCR back to Blur clears solid colors without recognizing again");
    controller.deactivate();
}

void displayedRecognitionSnapshotPreservesCachedResults() {
    ControllableOcrRecognition recognition;
    PromptRecorder recorder;
    auto controller = makeTextSession(recognition, recorder);
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 64, 64);
    presentation->lines.push_back(
        {QStringLiteral("Visible OCR"), 0.95,
         QPolygonF{QPointF(5, 5), QPointF(55, 5), QPointF(55, 25), QPointF(5, 25)}});
    presentation->prepareForRendering();
    ScreenshotRecognitionResults cached;
    cached.key = QStringLiteral("session");
    cached.text = ScreenshotOcrRecognitionResult{presentation, {}, {}, {}};
    SnowShotTableResult table;
    table.html = QStringLiteral("<table><tr><td>Cached table</td></tr></table>");
    cached.table = table;
    cached.qr = ScreenshotQrRecognitionResult{{QStringLiteral("Cached QR")}, {}};
    cached.translatedText = std::make_shared<ScreenshotOcrPresentation>();
    cached.translatedText->selection = presentation->selection;
    cached.translatedText->lines = presentation->lines;
    cached.translatedText->lines[0].text = QStringLiteral("Translated OCR");
    cached.translatedText->prepareForRendering();
    controller->seedRecognitionResults(cached);
    require(controller->recognitionResultsSnapshot().text->presentation->lines[0].text ==
                QStringLiteral("Visible OCR"),
            "an inactive session must fall back to its cached source result");
    controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    presentation->selectAll();
    const auto snapshot = controller->recognitionResultsSnapshot();
    controller->deactivate();
    require(
        snapshot.text.has_value() && snapshot.text->presentation != presentation &&
            snapshot.text->presentation->lines.front().text == QStringLiteral("Visible OCR") &&
            snapshot.text->presentation->selection == presentation->selection &&
            snapshot.text->presentation->lines.front().quad == presentation->lines.front().quad &&
            snapshot.text->presentation->selectedText().isEmpty(),
        "capturing before deactivation must preserve visible text and geometry without selection");
    require(snapshot.table.has_value() && snapshot.table->html == table.html &&
                snapshot.qr.has_value() && snapshot.qr->contents == cached.qr->contents &&
                snapshot.translatedText != nullptr &&
                snapshot.translatedText != cached.translatedText &&
                snapshot.translatedText->lines[0].text == QStringLiteral("Translated OCR"),
            "a display snapshot must preserve the session's table and QR results");
    controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    controller->beginTextEditing();
    require(controller->editing() &&
                controller->recognitionResultsSnapshot().text->presentation->lines[0].text ==
                    QStringLiteral("Visible OCR"),
            "text editor panels must snapshot cached OCR instead of transient editor content");
    require(recognition.requests == 0, "capturing cached display results must not request OCR");
    controller->endTextEditing();
    require(controller->activateCachedTextTranslation() &&
                controller->activateCachedTextTranslation() &&
                controller->originalImageTranslationActive() &&
                controller->originalText() == QStringLiteral("Visible OCR") &&
                controller->textDraft() == QStringLiteral("Translated OCR"),
            "restoring cached translation must be idempotent and retain source OCR");
    controller->endTextEditing();
    require(controller->textDraft() == QStringLiteral("Visible OCR") &&
                controller->recognitionResultsSnapshot().translatedText != nullptr,
            "canceling translation must restore the source while retaining its translation cache");
    const snow_shot::storage::ScreenshotTranslationSettings translationSettings;
    const auto previousConfiguration = translationSettings.configuration();
    auto hidden = makeTextSession(recognition, recorder);
    hidden->seedRecognitionResults(cached);
    int invalidatedResults = 0;
    QObject::connect(controller.get(),
                     &ScreenshotRecognitionSessionController::recognitionResultsChanged,
                     [&]() { ++invalidatedResults; });
    require(translationSettings.setConfiguration(
                {QStringLiteral("ja"), QStringLiteral("en"), QStringLiteral("qwen")}),
            "change translation settings after restoring a captured translation");
    require(
        controller->recognitionResultsSnapshot().translatedText == nullptr &&
            controller->textDraft() == QStringLiteral("Visible OCR") && invalidatedResults > 0,
        "an explicit translation settings change must invalidate the capture without changing OCR");
    hidden->activate(ScreenshotRecognitionSessionController::Mode::Text);
    hidden->beginTextTranslation();
    require(!hidden->activateCachedTextTranslation() &&
                hidden->originalText() == QStringLiteral("Visible OCR"),
            "first activation of a hidden translation must honor changed translation settings");
    hidden->deactivate();
    require(translationSettings.setConfiguration(previousConfiguration),
            "restore translation settings after the invalidation test");
    for (const bool wrongGeometry : {false, true}) {
        auto invalid = makeTextSession(recognition, recorder);
        if (wrongGeometry) {
            cached.translatedText->lines = presentation->lines;
            cached.translatedText->lines[0].quad.translate(QPointF(1, 0));
        } else {
            cached.translatedText->lines.clear();
        }
        invalid->seedRecognitionResults(cached);
        invalid->activate(ScreenshotRecognitionSessionController::Mode::Text);
        require(
            !invalid->activateCachedTextTranslation() && invalid->hasTextResult() &&
                invalid->recognitionResultsSnapshot().translatedText == nullptr,
            "translation with mismatched source lines or coordinates must not replace valid OCR");
    }
}

void liveDownloadsStillSurfaceThePrompt() {
    ControllableOcrRecognition recognition;
    PromptRecorder recorder;
    auto controller = makeTextSession(recognition, recorder);
    controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(recorder.modelDownloadShows == 0,
            "verification before the download starts must stay silent");
    processFor(250);
    require(recorder.modelDownloadShows == 0,
            "verification before the download starts must stay silent");

    recognition.m_status = {ScreenshotOcrAssetPhase::Downloading, QStringLiteral("models"),
                            qint64(25), qint64(100)};
    processFor(250);
    require(recorder.modelDownloadShows >= 1,
            "an active model download must surface the download prompt");
    require(recorder.modelDownloadMessages.last() ==
                QStringLiteral("Preparing text recognition components (25%)"),
            "the download prompt should report download progress");
    const int recognitionShowsDuringDownload = recorder.recognitionShows;

    recognition.m_ready = true;
    recognition.m_status = {ScreenshotOcrAssetPhase::ReadyCached, QStringLiteral("assets")};
    processFor(250);
    require(recorder.modelDownloadHides == 1,
            "a completed download should hide the download prompt");
    require(recorder.recognitionShows == recognitionShowsDuringDownload + 1,
            "the plain recognition message should return once the download finishes");

    const int showsAfterDownload = recorder.modelDownloadShows;
    recognition.completeWithEmptyPresentation();
    processFor(250);
    require(recorder.modelDownloadShows == showsAfterDownload && recorder.modelDownloadHides >= 1,
            "recognition completion must not leave the download prompt behind");
}

// Pinned windows prefetch recognition in the background; that prefetch must
// stay silent while assets verify but still report a real download.
void prefetchVerificationStaysSilentWhileDownloadsSurface() {
    ControllableOcrRecognition recognition;
    PromptRecorder recorder;
    auto controller = makeTextSession(recognition, recorder);
    controller->prefetchText();
    require(recognition.requests == 1, "prefetch should queue one request");
    require(recorder.modelDownloadShows == 0 && recorder.recognitionShows == 0,
            "an inactive prefetch during verification must stay silent");
    processFor(250);
    require(recorder.modelDownloadShows == 0,
            "an inactive prefetch during verification must stay silent");

    recognition.m_status = {ScreenshotOcrAssetPhase::Downloading, QStringLiteral("models"),
                            qint64(0), qint64(100)};
    processFor(250);
    require(recorder.modelDownloadShows >= 1,
            "a background download must still surface the download prompt");

    recognition.m_ready = true;
    recognition.m_status = {ScreenshotOcrAssetPhase::ReadyCached, QStringLiteral("assets")};
    processFor(250);
    require(recorder.modelDownloadHides == 1,
            "a completed background download should hide the download prompt");
    require(recorder.recognitionShows == 0,
            "an inactive prefetch must not show the recognition message");
    recognition.completeWithEmptyPresentation();
}

void translationLanguageSelectsUseCodePrefixGroups() {
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto previousConfiguration = settings.configuration();
    require(settings.setLayoutProcessing(QStringLiteral("original")),
            "select Original layout before opening language settings");
    QApplication::setQuitOnLastWindowClosed(false);
    QWidget owner;
    SnowShotApiClient apiClient(QStringLiteral("http://127.0.0.1:1"));
    ScreenshotRecognitionSessionActions actions;
    actions.translationSettingsOwner = [&owner]() { return &owner; };
    auto controller = std::make_unique<ScreenshotRecognitionSessionController>(
        nullptr, nullptr, &apiClient, std::move(actions));

    controller->openTranslationSettings();
    auto* modal = controller->findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotTranslationSettingsModal"));
    require(modal != nullptr, "translation settings should create its modal");
    QWidget* content = modal->contentWidget();
    auto* source = content == nullptr ? nullptr
                                      : content->findChild<adqt::widgets::AdSelect*>(
                                            QStringLiteral("screenshotTranslationSourceLanguage"));
    auto* target = content == nullptr ? nullptr
                                      : content->findChild<adqt::widgets::AdSelect*>(
                                            QStringLiteral("screenshotTranslationTargetLanguage"));
    require(source != nullptr && target != nullptr,
            "translation settings should expose source and target language selects");
    auto* originalImage = content->findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("screenshotTranslationOriginalImage"));
    const bool previousOriginalImage = settings.originalImageTranslationEnabled();
    require(originalImage != nullptr && originalImage->isChecked() == previousOriginalImage,
            "translation settings should reflect the shared original image translation setting");
    originalImage->setChecked(!previousOriginalImage);
    require(settings.originalImageTranslationEnabled() == previousOriginalImage,
            "editing the original image toggle should wait for OK before saving");
    require(source->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool &&
                target->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool,
            "translation language selects should use Qt tool popups");

    const auto sourceOptions = source->options();
    const auto targetOptions = target->options();
    require(sourceOptions.size() == 13 && targetOptions.size() == 12,
            "language selects should contain the expected source and target options");
    require(sourceOptions.constFirst().value == QStringLiteral("auto") &&
                sourceOptions.constFirst().group.isEmpty(),
            "auto-detect should remain outside language groups");
    require(sourceOptions.at(1).group == QStringLiteral("A") &&
                sourceOptions.at(2).group == QStringLiteral("D") &&
                sourceOptions.at(3).group == QStringLiteral("E") &&
                sourceOptions.at(12).group == QStringLiteral("Z"),
            "source language options should group by the first character of their code");
    require(targetOptions.constFirst().group == QStringLiteral("A") &&
                targetOptions.constLast().group == QStringLiteral("Z"),
            "target language options should group by the first character of their code");

    adqt::widgets::AdSelect* service = nullptr;
    for (auto* select : content->findChildren<adqt::widgets::AdSelect*>()) {
        if (select != source && select != target)
            service = select;
    }
    require(service != nullptr, "language settings include a service selector");
    service->setOptions({{QStringLiteral("test-model"), QStringLiteral("Test model")}});
    service->setCurrentValue(QStringLiteral("test-model"));
    source->setCurrentValue(QStringLiteral("en"));
    target->setCurrentValue(QStringLiteral("zh-Hans"));
    modal->closeRequested(adqt::widgets::AdModal::CloseReason::OkAction);
    require(settings.layoutProcessing() == QStringLiteral("original") &&
                settings.configuration().modelId == QStringLiteral("test-model"),
            "accepting translation languages preserves the separate layout processing choice");
    require(settings.setConfiguration(previousConfiguration), "restore translation configuration");
    require(settings.originalImageTranslationEnabled() == !previousOriginalImage,
            "OK should persist the original image translation toggle");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller->openTranslationSettings();
    modal = controller->findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotTranslationSettingsModal"));
    require(modal != nullptr, "translation settings should reopen after accepting");
    originalImage = modal->contentWidget()->findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("screenshotTranslationOriginalImage"));
    require(originalImage != nullptr && originalImage->isChecked() == !previousOriginalImage,
            "reopening the popup should load the saved toggle");
    originalImage->setChecked(previousOriginalImage);
    modal->closeRequested(adqt::widgets::AdModal::CloseReason::CancelAction);
    require(settings.originalImageTranslationEnabled() == !previousOriginalImage,
            "Cancel should discard edits to the original image translation toggle");
    require(settings.setOriginalImageTranslationEnabled(previousOriginalImage),
            "restore original image translation setting");
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated recognition test storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create test executable directory");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({executable, temporary.path(), 60000})
                .success,
            "initialize recognition test storage");
    if (application.arguments().contains(QStringLiteral("--image-conversion-only"))) {
        runImageConversionTests();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    runOriginalImageTranslationTests();
    if (application.arguments().contains(QStringLiteral("--translation-only"))) {
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    cachedRecognitionUsesTheSelectedFillStyle();
    translationLanguageSelectsUseCodePrefixGroups();
    displayedRecognitionSnapshotPreservesCachedResults();
    cachedVerificationStaysSilent();
    liveDownloadsStillSurfaceThePrompt();
    prefetchVerificationStaysSilentWhileDownloadsSurface();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
