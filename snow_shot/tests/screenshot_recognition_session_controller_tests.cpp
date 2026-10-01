#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include <QTextEdit>
#include <QMimeData>
#include <QJsonArray>
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/modal.h"
#include "widgets/popover.h"
#include "widgets/detail/overlay_popup_controller.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QPushButton>
#include <QSemaphore>
#include <QThreadPool>
#include <atomic>
#include "widgets/select.h"
#include "widgets/switch.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QScreen>
#include <QScopeGuard>

#include <iostream>
#include <memory>
#include <utility>

void runOriginalImageTranslationTests();
void runImageConversionTests();

class SnowShotApiClientTestAccess {
  public:
    static void prepare(SnowShotApiClient& client,
                        std::function<QByteArray(const QImage&)> callback) {
        client.m_tableImagePreparation = std::move(callback);
    }
};

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

void headlessWorkflowResultsAndEdits() {
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    QImage image(80, 60, QImage::Format_ARGB32);
    image.fill(Qt::white);
    session.setTarget({QStringLiteral("mcp-headless"), image, QRectF(0, 0, 80, 60)});
    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("mcp-headless");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 80, 60);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Original text");
    line.confidence = 0.95;
    line.quad = QPolygonF{QPointF(0, 0), QPointF(70, 0), QPointF(70, 20), QPointF(0, 20)};
    presentation->lines.append(line);
    presentation->prepareForRendering();
    results.text = ScreenshotOcrRecognitionResult{presentation};
    results.translatedText = std::make_shared<ScreenshotOcrPresentation>(*presentation);
    results.translatedText->setLineText(0, QStringLiteral("Translated text"));
    SnowShotTableResult table;
    table.html =
        QStringLiteral("<table><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table>");
    results.table = table;
    results.qr = ScreenshotQrRecognitionResult{{QStringLiteral("payload")}, {}};
    session.seedRecognitionResults(results);
    session.activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(session.workflowResult().value(QStringLiteral("lines")).toArray().size() == 1,
            "headless OCR returns layout");
    require(session.activateCachedTextTranslation(), "headless cached translation activates");
    const auto translated = session.workflowResult();
    require(translated.value(QStringLiteral("text")) == QStringLiteral("Translated text") &&
                translated.value(QStringLiteral("lines"))
                        .toArray()
                        .at(0)
                        .toObject()
                        .value(QStringLiteral("text")) == QStringLiteral("Translated text"),
            "headless translation exports matching text and layout");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_text")},
                                  {QStringLiteral("text"), QStringLiteral("Edited text")}}),
            "headless text edit");
    require(session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("Edited text"),
            "edited draft exported");
    session.activate(ScreenshotRecognitionSessionController::Mode::Table);
    require(session.workflowResult().value(QStringLiteral("cells")).toArray().size() == 4,
            "headless table returns cells");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("select_cells")},
                                  {QStringLiteral("range"), QJsonArray{0, 0, 0, 1}}}),
            "table selection without widget");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("merge_cells")}}),
            "headless table merge");
    require(session.workflowResult().value(QStringLiteral("cells")).toArray().size() == 3,
            "merged cells reflected in result");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}),
            "headless table undo");
    require(session.workflowResult().value(QStringLiteral("cells")).toArray().size() == 4,
            "table undo restores cells");
    const auto before = session.workflowResult();
    require(!session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                   {QStringLiteral("row"), 99},
                                   {QStringLiteral("column"), 0},
                                   {QStringLiteral("text"), QStringLiteral("invalid")}}),
            "invalid cell rejected");
    require(session.workflowResult() == before, "invalid cell edit is atomic");
    session.activate(ScreenshotRecognitionSessionController::Mode::Qr);
    require(session.workflowResult().value(QStringLiteral("contents")).toArray() ==
                QJsonArray{QStringLiteral("payload")},
            "headless QR result");
}
void tablePreparationPreservesSessionAndSiblingPopovers() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    for (int scenario = 0; scenario < 5; ++scenario) {
        QTcpServer server;
        require(server.listen(QHostAddress::LocalHost), "session HTTP server listens");
        SnowShotApiClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            auto* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                socket->readAll();
                const QByteArray body = R"({"data":{"html":"<table><tr><td>1</td></tr></table>"}})";
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        QSemaphore entered, release;
        std::atomic<int> preparations = 0;
        SnowShotApiClientTestAccess::prepare(client, [&](const QImage& image) {
            ++preparations;
            entered.release();
            release.acquire();
            return scenario == 4 ? SnowShotApiClient::encodeWebp(image) : QByteArray();
        });
        QEventLoop completionLoop;
        bool tableBusy = false;
        int errors = 0;
        ScreenshotRecognitionSessionActions actions;
        actions.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
        actions.setBusyState = [&](bool, bool table, bool) {
            tableBusy = table;
            if (!table) {
                completionLoop.quit();
            }
        };
        actions.showStatus = [&](const QString&, bool error) { errors += error ? 1 : 0; };
        ScreenshotRecognitionSessionController session(nullptr, nullptr, &client, actions);
        QImage image(32, 32, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        session.setTarget({QStringLiteral("first"), image, QRectF(0, 0, 32, 32)});
        QWidget host;
        host.resize(640, 360);
        QPushButton first(QStringLiteral("Table"), &host), second(QStringLiteral("Sibling"), &host);
        first.setGeometry(50, 100, 100, 32);
        second.setGeometry(180, 100, 100, 32);
        adqt::widgets::AdPopover firstPopup, secondPopup;
        firstPopup.setSourceWidget(&first);
        secondPopup.setSourceWidget(&second);
        firstPopup.setText(QStringLiteral("Table options"));
        secondPopup.setText(QStringLiteral("Sibling options"));
        host.show();
        const auto verifyPopup = [&](adqt::widgets::AdPopover& popup) {
            popup.show();
            require(popup.isVisible(), "sibling popup remains logically openable");
            auto* controller = popup.findChild<adqt::widgets::detail::OverlayPopupController*>();
            auto* surface = controller ? controller->delegate()->popupSurfaceWidget() : nullptr;
            require(surface && surface->isVisible(),
                    "requested popup surface must actually display");
            popup.hide();
        };
        verifyPopup(firstPopup);
        QObject::connect(&first, &QPushButton::clicked, &session,
                         [&]() { session.activate(Mode::Table); });
        first.click();
        require(tableBusy && session.busy(Mode::Table),
                "table becomes busy before encoding completes");
        require(entered.tryAcquire(1, 5000), "table worker starts");
        session.activate(Mode::Table);
        require(preparations == 1, "repeated activation does not duplicate pending preparation");
        verifyPopup(secondPopup);
        if (scenario == 1) {
            session.invalidate();
        }
        if (scenario == 2) {
            session.setTarget({QStringLiteral("second"), image, QRectF(0, 0, 32, 32)});
        }
        if (scenario == 3) {
            ScreenshotRecognitionResults cached;
            cached.key = QStringLiteral("first");
            cached.qr = ScreenshotQrRecognitionResult{{QStringLiteral("Cached QR")}, {}};
            session.seedRecognitionResults(cached);
            session.activate(Mode::Qr);
        }
        release.release();
        require(QThreadPool::globalInstance()->waitForDone(5000), "session worker settles");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        if (session.busy(Mode::Table)) {
            QTimer::singleShot(5000, &completionLoop, &QEventLoop::quit);
            completionLoop.exec();
        }
        if (scenario == 4) {
            require(session.cachedRecognitionResults().table.has_value(),
                    "successful table result is retained");
        }
        require(!session.busy(Mode::Table), "terminal preparation clears table busy state");
        require(errors == (scenario == 0 ? 1 : 0),
                "stale or inactive failures do not affect current UI");
        verifyPopup(firstPopup);
        verifyPopup(secondPopup);
    }
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

void recognizedTextDefaultsApplyOnFirstEditAndOriginalCopy() {
    const snow_shot::storage::TextRecognitionSettings settings;
    const QString priorFormatting = settings.defaultFormatting();
    const QString priorPunctuation = settings.defaultPunctuation();
    require(settings.setDefaultFormatting(QStringLiteral("remove")) &&
                settings.setDefaultPunctuation(QStringLiteral("full")),
            "set OCR defaults for the recognition session");

    QString selectedFormatting;
    QString selectedPunctuation;
    ScreenshotRecognitionWindow window({});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&window]() { return &window; };
    actions.setTextTransformState = [&](const QString& formatting, const QString& punctuation) {
        selectedFormatting = formatting;
        selectedPunctuation = punctuation;
    };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    QImage image(80, 60, QImage::Format_ARGB32);
    image.fill(Qt::white);
    session.setTarget({QStringLiteral("defaults"), image, QRectF(0, 0, 80, 60)});
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 80, 60);
    presentation->lines = {
        ScreenshotOcrLine{QStringLiteral("A,"), 1.0, QPolygonF(QRectF(0, 0, 60, 20))},
        ScreenshotOcrLine{QStringLiteral("B!"), 1.0, QPolygonF(QRectF(0, 30, 60, 20))},
    };
    presentation->prepareForRendering();
    ScreenshotRecognitionResults cached;
    cached.key = QStringLiteral("defaults");
    cached.text = ScreenshotOcrRecognitionResult{presentation};
    cached.translatedText = std::make_shared<ScreenshotOcrPresentation>(*presentation);
    cached.translatedText->setLineText(0, QStringLiteral("Translated,"));
    cached.translatedText->setLineText(1, QStringLiteral("overlay!"));
    session.seedRecognitionResults(cached);
    session.activate(ScreenshotRecognitionSessionController::Mode::Text);
    session.setShowOriginalImage(true);
    require(window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() ==
                    QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("B") + QChar(0xFF01),
            "original-image window copy applies defaults before edit mode");
    require(session.recognitionClipboardMimeData()->text() ==
                    QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("B") + QChar(0xFF01) &&
                session.originalText() == QStringLiteral("A,\nB!"),
            "full original-image copy uses defaults without changing the OCR source");
    presentation->beginTextSelection(ScreenshotOcrTextPosition{0, 1});
    presentation->updateTextSelection(ScreenshotOcrTextPosition{1, 1});
    presentation->finishTextSelection();
    require(window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == QString(QChar(0xFF0C)) + QStringLiteral("B"),
            "original-image window selection copy applies defaults");
    require(session.recognitionClipboardMimeData(presentation.get())->text() ==
                QString(QChar(0xFF0C)) + QStringLiteral("B"),
            "selected original-image copy uses the same defaults");
    presentation->clearTextSelection();
    session.beginTextEditing();
    require(session.textDraft() ==
                    QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("B") + QChar(0xFF01) &&
                selectedFormatting == QStringLiteral("remove") &&
                selectedPunctuation == QStringLiteral("full"),
            "first edit entry transforms the draft and selects both toolbar options");
    session.undoTextEdit();
    require(session.textDraft() == QStringLiteral("A,\nB!"),
            "one undo reverses both default transformations");
    session.redoTextEdit();
    session.setTextDraft(QStringLiteral("User\n?"));
    session.endTextEditing();
    session.beginTextEditing();
    require(session.textDraft() == QStringLiteral("User\n?"),
            "re-entering edit mode preserves the manually changed draft");
    session.endTextEditing();
    require(session.activateCachedTextTranslation() && session.originalImageTranslationActive() &&
                session.recognitionClipboardMimeData()->text() ==
                    QStringLiteral("Translated,\noverlay!"),
            "translated overlay copy bypasses OCR defaults");
    require(window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("Translated,\noverlay!"),
            "translated overlay window copy bypasses OCR defaults");
    session.setTarget({QStringLiteral("other"), image, QRectF(0, 0, 80, 60)});
    session.setTarget({QStringLiteral("defaults"), image, QRectF(0, 0, 80, 60)});
    session.activate(ScreenshotRecognitionSessionController::Mode::Text);
    session.beginTextEditing();
    require(session.textDraft() ==
                QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("B") + QChar(0xFF01),
            "discarding a target draft allows defaults on its next first edit entry");
    require(settings.setDefaultFormatting(priorFormatting) &&
                settings.setDefaultPunctuation(priorPunctuation),
            "restore OCR defaults after the recognition session");
}

// A cached launch pays asset re-verification and helper start-up before the
// first recognition can run; none of that may surface the download prompt.
void originalImagePreviewFollowsTextSessionLifecycle() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    const snow_shot::storage::TextRecognitionSettings settings;
    const bool previousEnabled = settings.showOriginalImagePreview();
    const snow_shot::storage::ScreenshotTranslationSettings translationSettings;
    const bool previousTranslationMode = translationSettings.originalImageTranslationEnabled();
    const auto restore = qScopeGuard([&]() {
        static_cast<void>(settings.setShowOriginalImagePreview(previousEnabled));
        static_cast<void>(
            translationSettings.setOriginalImageTranslationEnabled(previousTranslationMode));
    });
    require(settings.setShowOriginalImagePreview(true), "enable the preview fixture");
    QPointer<ScreenshotRecognitionWindow> window;
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() -> ScreenshotRecognitionWindow* {
        if (window == nullptr) {
            window = new ScreenshotRecognitionWindow({});
            require(window->present({QGuiApplication::primaryScreen(), nullptr,
                                     QRect(300, 180, 96, 72), QRectF(10, 20, 64, 48)}),
                    "present the recognition preview fixture");
        }
        return window.data();
    };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    const auto installTarget = [&](const QString& key, const QColor& color) {
        QImage image(64, 48, QImage::Format_ARGB32_Premultiplied);
        image.fill(color);
        session.setTarget({key, image, QRectF(10, 20, 64, 48)});
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = QRect(10, 20, 64, 48);
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Source text");
        line.quad = {QPointF(15, 25), QPointF(65, 25), QPointF(65, 45), QPointF(15, 45)};
        presentation->lines.push_back(line);
        presentation->prepareForRendering();
        ScreenshotRecognitionResults cached;
        cached.key = key;
        cached.text = ScreenshotOcrRecognitionResult{presentation, {}, {}, {}};
        session.seedRecognitionResults(std::move(cached));
    };
    const auto preview = [&]() -> QWidget* {
        return window != nullptr ? window->findChild<QWidget*>(
                                       QStringLiteral("screenshotOriginalImagePreviewWindow"))
                                 : nullptr;
    };
    const auto requirePreviewVisible = [&](const char* message) {
        processFor(5);
        require(window != nullptr && window->originalImagePreviewEnabled() &&
                    preview() != nullptr && preview()->isVisible(),
                message);
    };
    const auto requirePreviewDestroyed = [&](const char* message) {
        processFor(5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(preview() == nullptr, message);
    };
    installTarget(QStringLiteral("preview-first"), Qt::blue);
    session.activate(Mode::Text);
    requirePreviewVisible("default text recognition shows its original input preview");
    const QPointer<QWidget> firstPreview = preview();
    session.beginTextEditing();
    session.setTextDraft(QStringLiteral("Edited source"));
    require(session.editing(), "source editor fixture is active");
    requirePreviewVisible("source text editing retains the preview");
    session.endTextEditing();
    require(translationSettings.setOriginalImageTranslationEnabled(true),
            "select image translation fixture");
    session.beginTextTranslation();
    require(session.originalImageTranslationActive(), "image translation fixture is active");
    requirePreviewVisible("original-image translation retains the original input preview");
    session.endTextEditing();
    require(translationSettings.setOriginalImageTranslationEnabled(false),
            "select translation editor fixture");
    session.beginTextTranslation();
    require(session.translating() && session.editing() && !session.originalImageTranslationActive(),
            "translation editor fixture is active");
    requirePreviewVisible("translation editing retains the original input preview");
    require(preview() == firstPreview, "visible text submodes reuse the same companion window");
    session.setShowOriginalImage(true);
    requirePreviewDestroyed("original-image viewing destroys the separate companion");
    require(firstPreview.isNull(), "original-image viewing releases the preceding companion");
    session.setShowOriginalImage(false);
    requirePreviewVisible("leaving original-image viewing creates a new companion");
    const QPointer<QWidget> suppressedPreview = preview();
    window->setOriginalImagePreviewSuppressed(true);
    requirePreviewDestroyed("temporary host interaction destroys the preview");
    require(suppressedPreview.isNull(), "suppression releases the preceding companion window");
    require(settings.setShowOriginalImagePreview(false), "disable preview during host interaction");
    window->setOriginalImagePreviewSuppressed(false);
    require(!window->originalImagePreviewEnabled(), "live setting change disables the preview");
    requirePreviewDestroyed("interaction completion does not override a disabled preference");
    window->setOriginalImagePreviewSuppressed(true);
    require(settings.setShowOriginalImagePreview(true), "enable preview during host interaction");
    requirePreviewDestroyed("enabling the preference respects interaction suppression");
    window->setOriginalImagePreviewSuppressed(false);
    requirePreviewVisible("interaction completion restores an enabled preview");
    window->hide();
    requirePreviewDestroyed("hiding the recognition host destroys its preview");
    window->show();
    requirePreviewVisible("showing an active recognition host restores its preview");
    session.activate(Mode::Qr);
    require(!window->originalImagePreviewEnabled(), "nontext recognition disables the preview");
    requirePreviewDestroyed("nontext recognition cannot retain a text preview");
    session.activate(Mode::Text);
    requirePreviewVisible("returning to text recognition restores its preview");
    session.deactivate();
    require(!window->originalImagePreviewEnabled(), "deactivation disables the preview");
    requirePreviewDestroyed("deactivation cannot retain a separate preview");
    installTarget(QStringLiteral("preview-second"), Qt::red);
    session.activate(Mode::Text);
    requirePreviewVisible("a replacement target restores its own preview");
    const QImage displayed = preview()->grab().toImage();
    require(displayed.pixelColor(displayed.width() / 2, displayed.height() / 2) == QColor(Qt::red),
            "preview content must come from the replacement original image");
    const QPointer<QWidget> replacedPreview = preview();
    delete window.data();
    require(window == nullptr && replacedPreview == nullptr,
            "destroying recognition content also destroys its separate companion");
    session.activate(Mode::Text);
    requirePreviewVisible("recreated recognition content restores active preview state");
    session.invalidate();
    requirePreviewDestroyed("invalidation tears down the active preview");
    delete window.data();
    ScreenshotRecognitionSessionActions headlessActions;
    headlessActions.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
    ScreenshotRecognitionSessionController headless(nullptr, nullptr, nullptr, headlessActions);
    QImage headlessImage(8, 8, QImage::Format_ARGB32_Premultiplied);
    headlessImage.fill(Qt::green);
    headless.setTarget({QStringLiteral("preview-headless"), headlessImage, QRectF(0, 0, 8, 8)});
    headless.activate(Mode::Text);
    require(headless.active(), "headless text recognition remains usable with preview enabled");
}

void originalImageOverridePreservesSessionState() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    ControllableOcrRecognition recognition;
    ScreenshotRecognitionWindow window({});
    window.resize(320, 200);
    window.show();
    bool original = false;
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    actions.setShowOriginalImage = [&](bool show) { original = show; };
    ScreenshotRecognitionSessionController session(&recognition, nullptr, nullptr, actions);
    QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    session.setTarget({QStringLiteral("original-toggle"), image, QRectF(0, 0, 64, 64)});
    session.activate(Mode::Text);
    session.setShowOriginalImage(true);
    recognition.completeWithEmptyPresentation();
    auto* content = window.findChild<QWidget*>(QStringLiteral("screenshotRecognitionContent"));
    require(session.showOriginalImage() && original && content->isHidden(),
            "asynchronous recognition completion cannot reveal hidden content");
    session.beginTextEditing();
    session.setTextDraft(QStringLiteral("Edited OCR"));
    auto* editor = window.findChild<QTextEdit*>();
    require(session.editing() && editor && !editor->isVisible(),
            "original image takes precedence over newly activated edit mode");
    session.setShowOriginalImage(false);
    require(session.editing() && editor->isVisible() &&
                session.textDraft() == QStringLiteral("Edited OCR"),
            "revealing content preserves the editor and draft");
    session.setShowOriginalImage(true);
    const auto clipboard = session.recognitionClipboardMimeData();
    require(clipboard && clipboard->text() == QStringLiteral("Edited OCR"),
            "show original image preserves recognized-text clipboard commands");
    session.setShowOriginalImage(false);
    require(window.findChild<QTextEdit*>() == editor && session.editing(),
            "toggling never rebuilds the text editor");
    session.undoTextEdit();
    require(session.textDraft().isEmpty(), "text undo history survives original-image viewing");
    session.redoTextEdit();
    require(session.textDraft() == QStringLiteral("Edited OCR"), "redo history survives");
    require(recognition.requests == 1, "toggling never repeats recognition");
    for (const auto mode : {Mode::Table, Mode::Qr, Mode::Markdown, Mode::Html, Mode::Text}) {
        session.setShowOriginalImage(true);
        session.activate(mode);
        require(!session.showOriginalImage() && !original && !content->isHidden(),
                "switching recognition tools resets original-image mode");
    }
    session.setShowOriginalImage(true);
    session.deactivate();
    require(!session.showOriginalImage() && !original, "deactivation resets original-image mode");
    session.activate(Mode::Text);
    session.setShowOriginalImage(true);
    session.setTarget({QStringLiteral("next-image"), image, QRectF(0, 0, 64, 64)});
    require(!session.showOriginalImage() && !original, "new target resets original-image mode");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 64, 64);
    presentation->lines.push_back(
        {QStringLiteral("Source"), 0.99,
         QPolygonF{QPointF(5, 5), QPointF(55, 5), QPointF(55, 25), QPointF(5, 25)}});
    presentation->prepareForRendering();
    ScreenshotRecognitionResults cached;
    cached.key = QStringLiteral("next-image");
    cached.text = ScreenshotOcrRecognitionResult{presentation, {}, {}, {}};
    cached.translatedText = std::make_shared<ScreenshotOcrPresentation>(*presentation);
    cached.translatedText->setLineText(0, QStringLiteral("Translated OCR"));
    session.seedRecognitionResults(cached);
    session.activate(Mode::Text);
    require(session.activateCachedTextTranslation(), "activate cached translation fixture");
    require(session.workflowResult().value(QStringLiteral("text")) ==
                QStringLiteral("Translated OCR"),
            "workflow exports in-image translation through the active text draft");
    session.setShowOriginalImage(true);
    require(session.translating() && session.originalImageTranslationActive() &&
                !session.originalImageVisible() && content->isHidden() &&
                session.textDraft() == QStringLiteral("Translated OCR"),
            "original-image override preserves translation without exporting its overlay");
    session.setShowOriginalImage(false);
    require(session.originalImageVisible() && session.translating() && !content->isHidden(),
            "disabling original-image override restores translation presentation");
}

void deactivationNotifiesOnlyOnStateTransition() {
    ScreenshotRecognitionWindow content({});
    int visualExits = 0;
    int modeExits = 0;
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &content; };
    actions.setRecognitionVisualState = [&](bool active) { visualExits += !active; };
    actions.setActiveMode = [&](int mode) { modeExits += mode == -1; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    session.setTarget({QStringLiteral("deactivation"), image, QRectF(0, 0, 32, 32)});
    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("deactivation");
    results.qr = ScreenshotQrRecognitionResult{{QStringLiteral("Cached QR")}, {}};
    session.seedRecognitionResults(results);
    for (int cycle = 1; cycle <= 2; ++cycle) {
        session.activate(ScreenshotRecognitionSessionController::Mode::Qr);
        session.deactivate();
        require(!session.active() && visualExits == cycle && modeExits == cycle,
                "leaving an active session must notify the host exactly once");
        session.deactivate();
        session.deactivate();
        require(visualExits == cycle && modeExits == cycle,
                "retained recognition content must not cause repeated exit notifications");
    }
}

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
    require(!controller->originalImageVisible(),
            "inactive recognition is not an original-image view");
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
    require(controller->originalImageVisible(), "active OCR is an original-image view");
    presentation->selectAll();
    const auto snapshot = controller->recognitionResultsSnapshot();
    controller->deactivate();
    require(!controller->originalImageVisible(),
            "deactivation immediately excludes recognition export");
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
    require(controller->editing() && !controller->originalImageVisible() &&
                controller->sourceTextDraft() == QStringLiteral("Visible OCR") &&
                controller->recognitionResultsSnapshot().text->presentation->lines[0].text ==
                    QStringLiteral("Visible OCR"),
            "text editor panels must snapshot cached OCR instead of transient editor content");
    controller->setTextDraft(QStringLiteral("Edited\nOCR"));
    controller->applyTextFormatting(QStringLiteral("remove"));
    require(controller->sourceTextDraft() == QStringLiteral("EditedOCR"),
            "translation-page source must include current OCR edits and transforms");
    require(recognition.requests == 0, "capturing cached display results must not request OCR");
    controller->endTextEditing();
    require(controller->activateCachedTextTranslation() &&
                controller->activateCachedTextTranslation() &&
                controller->originalImageTranslationActive() &&
                controller->originalImageVisible() &&
                controller->originalText() == QStringLiteral("Visible OCR") &&
                controller->sourceTextDraft() == QStringLiteral("EditedOCR") &&
                controller->textDraft() == QStringLiteral("Translated OCR"),
            "restoring cached translation must retain the edited source separately from output");
    controller->endTextEditing();
    require(controller->textDraft() == QStringLiteral("EditedOCR") &&
                controller->sourceTextDraft() == QStringLiteral("EditedOCR") &&
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
            controller->textDraft() == QStringLiteral("EditedOCR") &&
            controller->sourceTextDraft() == QStringLiteral("EditedOCR") && invalidatedResults > 0,
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
    const snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Test model"),
        QStringLiteral("http://127.0.0.1:1"),
        {},
        QStringLiteral("test-model"),
        true};
    const auto previousModels = snow_shot::storage::ApiConfigurationSettings().customModels();
    require(snow_shot::storage::ApiConfigurationSettings().setCustomModels({model}),
            "configure custom model through the shared settings boundary");
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

    for (auto* select : content->findChildren<adqt::widgets::AdSelect*>()) {
        require(select->searchEnabled(), "translation settings selects support input filtering");
        const auto previous = select->currentValue();
        select->setSearchText(QStringLiteral("no-matching-translation-option-1937"));
        require(select->currentValue() == previous,
                "filtering translation options cannot change the value");
        select->setSearchText(QString());
    }

    const auto sourceOptions = source->options();
    const auto targetOptions = target->options();
    require(sourceOptions.size() == 14 && targetOptions.size() == 13,
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
    require(service->isEnabled(), "configured custom models enable the service selector");
    service->setCurrentValue(model.selectionId());
    source->setCurrentValue(QStringLiteral("en"));
    target->setCurrentValue(QStringLiteral("zh-Hans"));
    modal->closeRequested(adqt::widgets::AdModal::CloseReason::OkAction);
    require(settings.layoutProcessing() == QStringLiteral("original") &&
                settings.configuration().modelId == model.selectionId(),
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
    require(snow_shot::storage::ApiConfigurationSettings().setCustomModels(previousModels),
            "restore custom models");
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
} // namespace

void runLatexRecognitionTests();

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
    if (application.arguments().contains(QStringLiteral("--latex-only"))) {
        runLatexRecognitionTests();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--original-image-only"))) {
        originalImageOverridePreservesSessionState();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--original-image-preview-only"))) {
        originalImagePreviewFollowsTextSessionLifecycle();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-defaults-only"))) {
        recognizedTextDefaultsApplyOnFirstEditAndOriginalCopy();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--table-only"))) {
        tablePreparationPreservesSessionAndSiblingPopovers();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
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
    headlessWorkflowResultsAndEdits();
    originalImagePreviewFollowsTextSessionLifecycle();
    deactivationNotifiesOnlyOnStateTransition();
    tablePreparationPreservesSessionAndSiblingPopovers();
    cachedRecognitionUsesTheSelectedFillStyle();
    translationLanguageSelectsUseCodePrefixGroups();
    displayedRecognitionSnapshotPreservesCachedResults();
    cachedVerificationStaysSilent();
    liveDownloadsStillSurfaceThePrompt();
    prefetchVerificationStaysSilentWhileDownloadsSurface();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
