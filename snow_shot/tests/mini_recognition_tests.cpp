#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotimageconversionpersistence.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QDir>
#include <QDataStream>
#include <QMimeData>
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

class QrRecognitionProbe final : public ScreenshotQrRecognitionPort {
  public:
    int requests = 0;
    int cancellations = 0;

    RequestToken recognize(QImage, QObject*, Completion, ScreenshotQrRecognitionMode) override {
        return static_cast<RequestToken>(++requests);
    }
    void cancel(RequestToken) override {
        ++cancellations;
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
    require(!session.fileExportSnapshot(),
            "Mini text recognition must keep image saving instead of removed text exports");
    const auto copied = session.recognitionClipboardMimeData();
    require(copied && copied->text() == QStringLiteral("Recognized locally"),
            "Mini manual OCR must preserve plain text clipboard export");
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
    results.table.emplace().html = QStringLiteral("<table><tr><td>x</td></tr></table>");
    results.qr.emplace().contents = {QStringLiteral("qr")};
    results.latex.emplace().latex = QStringLiteral("x^2");
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

void removedProvidersAndCachesDoNotParticipateInTextSessions() {
    LocalTextRecognition recognition;
    QrRecognitionProbe qr;
    ScreenshotRecognitionSessionController session(&recognition, &qr, nullptr, {});
    for (QObject* child : session.findChildren<QObject*>()) {
        require(QByteArray(child->metaObject()->className()) !=
                    QByteArrayLiteral("ScreenshotImageConversionController"),
                "Mini text sessions must not instantiate a disabled conversion controller");
    }

    QImage image(120, 40, QImage::Format_ARGB32);
    image.fill(Qt::white);
    const QString key = QStringLiteral("mini-legacy-recognition");
    session.setTarget({key, image, QRectF(0, 0, 120, 40)});
    ScreenshotRecognitionResults legacy;
    legacy.key = key;
    legacy.table.emplace().html = QStringLiteral("<table><tr><td>x</td></tr></table>");
    legacy.qr.emplace().contents = {QStringLiteral("qr")};
    legacy.latex.emplace().latex = QStringLiteral("x^2");
    legacy.visibleLatex = true;
    legacy.conversions.append({SnowShotImageConversionFormat::Markdown, QStringLiteral("model"),
                               QStringLiteral("# Converted")});
    legacy.visibleConversion = SnowShotImageConversionFormat::Markdown;
    session.seedRecognitionResults(legacy);
    require(session.cachedRecognitionResults().isEmpty() && !session.fileExportSnapshot(),
            "Mini must discard imported results for all removed recognition modes");

    using Mode = ScreenshotRecognitionSessionController::Mode;
    session.activate(Mode::Text);
    for (const auto mode : {Mode::Table, Mode::Qr, Mode::Latex, Mode::Markdown, Mode::Html}) {
        session.activate(mode);
        require(session.active() && session.mode() == Mode::Text && !session.busy(mode),
                "removed modes must preserve an active Mini text session");
    }
    session.openImageConversionSettings();
    session.cancelWorkflow();
    session.invalidate();
    require(recognition.requests == 1 && qr.requests == 0 && qr.cancellations == 0,
            "Mini must never dispatch or cancel requests to an injected disabled QR provider");
}

void removedRecognitionViewsRemainUnavailable() {
    ScreenshotRecognitionWindow window(
        {}, nullptr, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    window.setTableSession({});
    window.showImageConversion(SnowShotImageConversionFormat::Markdown, QStringLiteral("# Text"),
                               false, {});
    window.showQrContents({QStringLiteral("https://example.invalid")});
    window.showQrContents({QStringLiteral("x^2")}, false);
    require(window.findChild<QWidget*>(QStringLiteral("snowShotRecognizedTable")) == nullptr &&
                window.findChild<QWidget*>(QStringLiteral("screenshotImageConversionView")) ==
                    nullptr &&
                window.findChild<QWidget*>(QStringLiteral("screenshotQrContents")) == nullptr,
            "Mini shared recognition window must not construct removed recognition views");
}

void legacyPinPayloadsRetainOnlySupportedRecognition() {
    for (const quint8 translationVersion : {quint8(1), quint8(2)}) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_11);
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Legacy local text");
        line.confidence = 0.99;
        line.quad = QPolygonF{QPointF(0, 0), QPointF(100, 0), QPointF(100, 20), QPointF(0, 20)};
        const QRect selection(0, 0, 120, 40);
        stream << QStringLiteral("legacy-pin") << quint8(1) << quint8(1) << quint8(1) << QString()
               << selection << qint64(1) << line.text << line.confidence << line.quad << quint8(0);
        stream << QStringLiteral("<table><tr><td>Unavailable</td></tr></table>") << QString()
               << QString() << int(0) << QStringList{QStringLiteral("Unavailable QR")} << QString();
        const QByteArray conversion = QByteArrayLiteral("Legacy conversion bytes");
        stream << snow_shot::presentation::kImageConversionPayloadMarker
               << snow_shot::presentation::kImageConversionPayloadVersion
               << quint32(conversion.size());
        stream.writeRawData(conversion.constData(), static_cast<int>(conversion.size()));
        stream << quint32(0x4C415458) << quint8(1) << QStringLiteral("x^2") << true;
        stream << quint32(0x53535452) << translationVersion;
        if (translationVersion == 1) {
            stream << QStringList{QStringLiteral("Unavailable translation")};
        } else {
            stream << selection << qint64(1) << QStringLiteral("Unavailable translation")
                   << line.confidence << line.quad << quint8(0) << line.paragraph
                   << line.sourceLineQuads;
        }
        require(stream.status() == QDataStream::Ok, "prepare a legacy Full recognition payload");
        const auto results = ScreenshotPinnedWindow::decodeRecognitionSnapshot(bytes);
        require(results.key == QStringLiteral("legacy-pin") && results.text &&
                    results.text->presentation && !results.text->presentation->lines.isEmpty() &&
                    results.text->presentation->lines.front().text == line.text && !results.table &&
                    !results.qr && !results.latex && !results.visibleLatex &&
                    !results.translatedText && results.conversions.isEmpty() &&
                    !results.visibleConversion,
                "Mini pin restore must skip removed payloads while retaining local text");
    }
}

void removedPaletteToolsCannotMaterializeControls() {
    ScreenshotToolPalette::Options options;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.showImageConversionTools = true;
    ScreenshotToolPalette palette(options);
    using Tool = ScreenshotToolPalette::Tool;
    palette.setActiveTool(Tool::Ocr);
    palette.setTextEditingState(true, true, true, true);
    require(palette.activeTool() == Tool::Ocr &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotOcrTextEditButton")) &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotOcrTextFormattingSelect")),
            "Mini manual OCR must retain editing and formatting controls");
    for (const auto tool :
         {Tool::TextTranslation, Tool::Table, Tool::Qr, Tool::Latex, Tool::Markdown, Tool::Html}) {
        palette.setActiveTool(tool);
        require(palette.activeTool() == Tool::Ocr,
                "removed recognition tools must not replace the active Mini OCR tool");
    }
    palette.setTextTranslationState(true, true, true, true, true, true);
    palette.setTableEditingState(true, true, true, true, true, true);
    palette.setImageConversionBusy(true, true);
    require(!palette.ensureActionFamily(ScreenshotToolPalette::ActionFamily::TableRecognition) &&
                !palette.ensureActionFamily(ScreenshotToolPalette::ActionFamily::ImageConversion),
            "Mini must reject direct requests to materialize removed recognition action families");
    require(palette.findChild<QWidget*>(QStringLiteral("screenshotOcrTextTranslateButton")) ==
                    nullptr &&
                palette.findChild<QWidget*>(QStringLiteral("screenshotTableMergeButton")) ==
                    nullptr &&
                palette.findChild<QWidget*>(
                    QStringLiteral("screenshotImageConversionSettingsButton")) == nullptr &&
                palette.activeTool() == Tool::Ocr,
            "disabled palette state must not create controls or change active OCR");
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
    removedProvidersAndCachesDoNotParticipateInTextSessions();
    removedRecognitionViewsRemainUnavailable();
    legacyPinPayloadsRetainOnlySupportedRecognition();
    removedPaletteToolsCannotMaterializeControls();
    storage.shutdown();
    return 0;
}
