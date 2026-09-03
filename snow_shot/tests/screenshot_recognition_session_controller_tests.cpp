#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <iostream>
#include <memory>
#include <utility>

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

    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override { return true; }

    bool modelFilesReady() const override { return m_ready; }

    ScreenshotOcrAssetStatus assetStatus() const override { return m_status; }

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
    ScreenshotOcrAssetStatus m_status{ScreenshotOcrAssetPhase::Verifying,
                                      QStringLiteral("assets")};

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

std::unique_ptr<ScreenshotRecognitionSessionController> makeTextSession(
    ControllableOcrRecognition& recognition, PromptRecorder& recorder) {
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
    require(recorder.modelDownloadShows == showsAfterDownload &&
                recorder.modelDownloadHides >= 1,
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
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    cachedVerificationStaysSilent();
    liveDownloadsStillSurfaceThePrompt();
    prefetchVerificationStaysSilentWhileDownloadsSurface();
    return 0;
}
