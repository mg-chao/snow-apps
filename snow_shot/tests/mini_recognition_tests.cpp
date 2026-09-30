#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class LocalTextRecognition final : public ScreenshotOcrRecognitionPort {
  public:
    int requests = 0;
    RequestToken recognize(ScreenshotOcrRequest request, QObject*, Completion completion) override {
        ++requests;
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = request.canvasRect.toAlignedRect();
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized locally");
        line.confidence = 0.99;
        line.quad = QPolygonF{QPointF(0, 0), QPointF(100, 0), QPointF(100, 20), QPointF(0, 20)};
        presentation->lines.append(line);
        presentation->prepareForRendering();
        completion(ScreenshotOcrRecognitionResult{presentation});
        return static_cast<RequestToken>(requests);
    }
    void cancel(RequestToken) override {}
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }
};

void manualRecognitionWithoutRemoteProviders() {
    LocalTextRecognition recognition;
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
    ScreenshotRecognitionSessionController session(&recognition, nullptr, nullptr, actions);
    QImage image(120, 40, QImage::Format_ARGB32);
    image.fill(Qt::white);
    session.setTarget({QStringLiteral("mini-manual-text"), image, QRectF(0, 0, 120, 40)});
    require(recognition.requests == 0, "creating a text session must not start recognition");
    using Mode = ScreenshotRecognitionSessionController::Mode;
    for (const auto mode : {Mode::Table, Mode::Qr, Mode::Latex, Mode::Markdown, Mode::Html}) {
        session.activate(mode);
        require(!session.active() && recognition.requests == 0,
                "removed modes must not activate providers or change session state");
    }
    session.activate(Mode::Text);
    require(session.hasTextResult() && recognition.requests == 1,
            "manual Mini text recognition must work without QR or API providers");
    require(session.workflowResult().value(QStringLiteral("text")).toString() ==
                QStringLiteral("Recognized locally"),
            "manual OCR must expose recognized text");
    session.beginTextTranslation();
    require(!session.translating() && !session.activateCachedTextTranslation(),
            "translation must remain unavailable within text recognition");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_text")},
                                  {QStringLiteral("text"), QStringLiteral("Edited locally")}}),
            "Mini recognized text must remain editable");
    require(session.workflowResult().value(QStringLiteral("text")).toString() ==
                QStringLiteral("Edited locally"),
            "edited Mini OCR text must remain exportable");

    auto results = session.cachedRecognitionResults();
    results.table = SnowShotTableResult{QStringLiteral("<table><tr><td>x</td></tr></table>")};
    results.qr = ScreenshotQrRecognitionResult{{QStringLiteral("qr")}, {}};
    results.latex = SnowShotLatexResult{QStringLiteral("x^2")};
    results.visibleLatex = true;
    results.translatedText = std::make_shared<ScreenshotOcrPresentation>();
    results.conversions.append({SnowShotImageConversionFormat::Markdown, QStringLiteral("model"),
                                QStringLiteral("# Converted")});
    results.visibleConversion = SnowShotImageConversionFormat::Markdown;
    sanitizeEditionRecognitionResults(results);
    require(results.text.has_value() && !results.table && !results.qr && !results.latex &&
                !results.visibleLatex && !results.translatedText && results.conversions.isEmpty() &&
                !results.visibleConversion,
            "legacy recognition payloads must retain text and discard removed feature state");
}

void removedRecognitionViewsRemainUnavailable() {
    ScreenshotRecognitionWindow window(
        {}, nullptr, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    window.setTableSession({});
    window.showImageConversion(SnowShotImageConversionFormat::Markdown, QStringLiteral("# Text"),
                               false, {});
    window.showQrContents({QStringLiteral("https://example.invalid")});
    require(window.findChild<QWidget*>(QStringLiteral("snowShotRecognizedTable")) == nullptr &&
                window.findChild<QWidget*>(QStringLiteral("screenshotImageConversionView")) ==
                    nullptr &&
                window.findChild<QWidget*>(QStringLiteral("screenshotQrContents")) == nullptr,
            "Mini shared recognition window must not construct removed recognition views");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    require(snow_shot::app::edition::isMini, "this test must be compiled as Mini");
    require(!ScreenshotPinnedWindow::Config{}.automaticTextRecognition,
            "Mini pinned windows do not request text recognition automatically");
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated Mini recognition storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create Mini test executable directory");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({executable, temporary.path(), 60000}).success,
            "initialize Mini recognition storage");
    manualRecognitionWithoutRemoteProviders();
    removedRecognitionViewsRemainUnavailable();
    storage.shutdown();
    return 0;
}
