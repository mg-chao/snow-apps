#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/translation/translationservice.h"
#include "widgets/modal.h"
#include "widgets/switch.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QApplication>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTextEdit>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextDocument>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

#if defined(SNOW_SHOT_TEST_PINNED_TRANSLATION)
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/context_menu.h"
#include "widgets/button.h"
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QScreen>
#include <QWheelEvent>
#endif

namespace {
// The built-in eight-slot policy is independently covered by TranslationService tests. Only
// scenarios that need queued work use this boundary; presentation fixtures use fixed small inputs.
constexpr int kBuiltInTranslationConcurrency = 8;

#if defined(SNOW_SHOT_TEST_PINNED_TRANSLATION)
QList<QGraphicsItem*> ocrGlyphItems(QGraphicsView* layer) {
    if (layer == nullptr || layer->scene() == nullptr) {
        return {};
    }
    auto items = layer->scene()->items();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const QGraphicsItem* item) { return item->zValue() != 0.0; }),
                items.end());
    return items;
}
#endif

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::_Exit(EXIT_FAILURE);
    }
}

void waitUntil(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}

void configureTranslation(bool overlay = true, const QString& model = QStringLiteral("model-a")) {
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    require(settings.setOriginalImageTranslationEnabled(overlay), "set translation display mode");
    require(settings.setConfiguration({QStringLiteral("en"), QStringLiteral("zh-Hans"), model,
                                       QStringLiteral("original")}),
            "set translation languages and model");
}

class TranslationServer final : public QObject {
  public:
    struct Stream {
        QPointer<QTcpSocket> socket;
        QJsonObject body;
        QString text;
        bool headersSent = false;
    };

    TranslationServer() {
        require(server.listen(QHostAddress::LocalHost), "listen on a local translation test port");
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (server.hasPendingConnections()) {
                QTcpSocket* socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket]() { readRequest(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }

    void waitForStreams(int count) {
        waitUntil([&]() { return streams.size() >= count; },
                  qPrintable(QStringLiteral("expected %1 translation requests").arg(count)));
        require(streams.size() == count, "translation queue must not dispatch extra requests");
        // Parallel HTTP connections can arrive in any order. Address each received batch
        // by its source text so response ordering is controlled by the test, not the OS.
        std::stable_sort(
            streams.begin() + orderedStreams, streams.end(),
            [](const Stream& first, const Stream& second) { return first.text < second.text; });
        orderedStreams = count;
    }

    static QByteArray deltaEvent(const QString& text) {
        const QJsonObject body{
            {QStringLiteral("choices"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), text}}}}}}};
        return QByteArrayLiteral("data: ") + QJsonDocument(body).toJson(QJsonDocument::Compact) +
               QByteArrayLiteral("\n\n");
    }

    void send(int index, const QByteArray& data) {
        const auto socket = streams.at(index).socket;
        require(socket != nullptr && socket->state() == QAbstractSocket::ConnectedState,
                "stream should remain connected");
        startStream(index);
        socket->write(data);
        socket->flush();
    }

    void delta(int index, const QString& text) {
        send(index, deltaEvent(text));
    }

    void finish(int index) {
        send(index, QByteArrayLiteral("data: [DONE]\n\n"));
        streams.at(index).socket->disconnectFromHost();
    }

    void fail(int index) {
        // Leave HTTP open: the client must close a terminal SSE error itself.
        send(index, QByteArrayLiteral("event: error\ndata: {\"message\":\"test failure\"}\n\n"));
    }

    void rateLimit(int index) {
        auto& stream = streams[index];
        require(!stream.headersSent && stream.socket != nullptr,
                "rate-limit response must precede stream headers");
        const QByteArray body = QByteArrayLiteral("{\"detail\":\"rate limited\"}");
        stream.headersSent = true;
        stream.socket->write(
            QByteArrayLiteral("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 60\r\n"
                              "Content-Type: application/json\r\nContent-Length: ") +
            QByteArray::number(body.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") +
            body);
        stream.socket->disconnectFromHost();
    }

    bool disconnected(int index) const {
        const auto socket = streams.at(index).socket;
        return socket == nullptr || socket->state() == QAbstractSocket::UnconnectedState;
    }

    QVector<Stream> streams;
    int modelRequests = 0;
    bool rejectModels = false;
    bool holdModels = false;
    bool holdStreamHeaders = false;

  private:
    void startStream(qsizetype index) {
        auto& stream = streams[index];
        if (stream.headersSent)
            return;
        stream.headersSent = true;
        stream.socket->write(QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"));
        stream.socket->flush();
    }

    void readRequest(QTcpSocket* socket) {
        if (socket->property("handled").toBool()) {
            return;
        }
        QByteArray input = socket->property("input").toByteArray() + socket->readAll();
        socket->setProperty("input", input);
        const qsizetype headerEnd = input.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        int contentLength = 0;
        for (const QByteArray& header : input.left(headerEnd).split('\n')) {
            if (header.toLower().startsWith("content-length:")) {
                contentLength = header.mid(header.indexOf(':') + 1).trimmed().toInt();
            }
        }
        if (input.size() - headerEnd - 4 < contentLength) {
            return;
        }
        socket->setProperty("handled", true);
        if (input.startsWith("GET ")) {
            ++modelRequests;
            if (holdModels) {
                return;
            }
            const QByteArray body =
                rejectModels
                    ? QByteArrayLiteral("{\"detail\":\"unavailable\"}")
                    : QByteArrayLiteral(
                          "{\"code\":0,\"data\":[{\"model\":\"model-a\",\"name\":\"Model A\","
                          "\"supports_vision\":false,\"translation_mode\":\"default\"},"
                          "{\"model\":\"qwen\",\"name\":\"Qwen\",\"supports_vision\":false,"
                          "\"translation_mode\":\"qwen-mt\"}]}");
            const QByteArray status =
                rejectModels ? QByteArrayLiteral("503 Unavailable") : QByteArrayLiteral("200 OK");
            socket->write(
                QByteArrayLiteral("HTTP/1.1 ") + status +
                QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ") +
                QByteArray::number(body.size()) +
                QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
            socket->disconnectFromHost();
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(input.mid(headerEnd + 4)).object();
        require(!body.isEmpty(), "translation request should contain a JSON object");
        const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
        streams.push_back(
            {socket, body, messages.last().toObject().value(QStringLiteral("content")).toString()});
        if (!holdStreamHeaders)
            startStream(streams.size() - 1);
    }

    QTcpServer server;
    int orderedStreams = 0;
};

struct SessionProbe {
    std::shared_ptr<ScreenshotOcrPresentation> source =
        std::make_shared<ScreenshotOcrPresentation>();
    std::shared_ptr<ScreenshotOcrPresentation> displayed;
    std::unique_ptr<ScreenshotRecognitionSessionController> controller;
    QStringList errors;
    QString primaryTarget;
    int textUpdates = 0;
    int backgrounds = 0;
    bool streaming = false;
    bool overlayState = false;
    std::function<QImage()> renderImage;

    ~SessionProbe() {
        controller.reset();
    }

    SessionProbe(SnowShotApiClient* api, int boxes, QString key = QStringLiteral("image"),
                 bool formatted = false, ScreenshotOcrRecognitionPort* ocr = nullptr,
                 QWidget* settingsOwner = nullptr,
                 ScreenshotRecognitionWindow* recognitionWindow = nullptr,
                 const QStringList& sourceTexts = {}) {
        ScreenshotRecognitionSessionActions actions;
        actions.renderTranslationImage = [this] { return renderImage ? renderImage() : QImage{}; };
        if (recognitionWindow != nullptr) {
            actions.ensureContent = [recognitionWindow]() { return recognitionWindow; };
        }
        actions.setTextTargetLanguage = [this](const QString& language) {
            primaryTarget = language;
        };
        actions.translationSettingsOwner = [settingsOwner]() { return settingsOwner; };
        actions.applyOcrPresentation = [this, recognitionWindow](auto presentation) {
            displayed = std::move(presentation);
            if (recognitionWindow != nullptr) {
                recognitionWindow->setOcrPresentation(displayed);
            }
        };
        actions.applyOcrBackground = [this](auto) { ++backgrounds; };
        if (ocr != nullptr) {
            actions.applyOcrBackgroundImage = [](auto, QImage, QRectF) {};
        }
        actions.updateOcrText = [this, recognitionWindow](int index, const QString& text) {
            ++textUpdates;
            require(displayed != source, "stream updates must not modify the source presentation");
            if (recognitionWindow != nullptr) {
                recognitionWindow->updateOcrText(index, text);
            } else {
                displayed->setLineText(index, text);
            }
        };
        actions.showStatus = [this](const QString& text, bool error) {
            if (error) {
                errors.push_back(text);
            }
        };
        actions.setTextTranslationState = [this](bool, bool, bool busy, bool undo, bool redo,
                                                 bool reset, bool overlay) {
            streaming = busy;
            overlayState = overlay;
            if (overlay) {
                require(!undo && !redo && !reset, "overlay translation has no editing history");
            }
        };
        controller = std::make_unique<ScreenshotRecognitionSessionController>(ocr, nullptr, api,
                                                                              std::move(actions));
        const int imageHeight = std::max(240, boxes * 25);
        source->selection = QRect(0, 0, 160, imageHeight);
        for (int index = 0; index < boxes; ++index) {
            const qreal top = index * 25.0;
            source->lines.push_back(
                {sourceTexts.value(index, QStringLiteral("source %1").arg(index)), 0.95,
                 QPolygonF{QPointF(0, top), QPointF(150, top), QPointF(150, top + 20),
                           QPointF(0, top + 20)}});
        }
        source->prepareForRendering();
        ScreenshotRecognitionTarget target;
        target.key = key;
        target.image = QImage(160, imageHeight, QImage::Format_ARGB32_Premultiplied);
        target.image.fill(Qt::white);
        target.canvasRect = source->selection;
        if (formatted) {
            target.formattedTextDocument =
                std::make_shared<QTextDocument>(QStringLiteral("formatted source"));
        }
        controller->setTarget(std::move(target));
        ScreenshotOcrRecognitionResult text;
        text.presentation = source;
        ScreenshotRecognitionResults results;
        results.key = key;
        results.text = std::move(text);
        controller->seedRecognitionResults(std::move(results));
        controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    }
};

void translationImageCopyHonorsSelectionAndSnapshots() {
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setCopyTranslationResultsAsImage(false)); });
    for (const bool overlay : {true, false}) {
        configureTranslation(overlay);
        TranslationServer server;
        SnowShotApiClient api(server.url());
        ScreenshotRecognitionWindow window({});
        SessionProbe session(&api, 1, QStringLiteral("image-copy"), false, nullptr, nullptr,
                             &window);
        window.resize(240, 90);
        window.show();
        session.renderImage = [&] {
            QImage background(160, 240, QImage::Format_ARGB32_Premultiplied);
            background.fill(Qt::white);
            auto snapshot =
                window.imageSnapshot(background, session.source->selection, {}, {}, {}, true);
            return snapshot ? renderScreenshotRecognitionImage(*snapshot) : QImage{};
        };
        session.controller->beginTextTranslation();
        server.waitForStreams(1);
        const QString result = QString::fromUtf8("Translated text \xE4\xB8\xAD\xE6\x96\x87\n") +
                               QStringLiteral("Another translated paragraph\n").repeated(40);
        server.delta(0, result);
        waitUntil([&] { return session.controller->textDraft() == result; },
                  "stream image-copy result");
        require(session.controller->recognitionClipboardMimeData()->text() == result,
                "disabled image copying preserves translation text");
        const qsizetype requests = server.streams.size();
        require(settings.setCopyTranslationResultsAsImage(true),
                "enable translation image copying");
        auto* editor = window.findChild<QTextEdit*>();
        int scroll = 0;
        int revision = 0;
        if (!overlay) {
            require(editor != nullptr, "text translation owns an editor");
            editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum());
            scroll = editor->verticalScrollBar()->value();
            revision = editor->document()->revision();
        }
        auto mime = session.controller->recognitionClipboardMimeData();
        require(mime && mime->hasImage() && !mime->hasText(),
                "unselected translation copies only an image");
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        require(!image.isNull() && (overlay ? image.size() == QSize(160, 240)
                                            : image.height() > editor->viewport()->height()),
                "image export covers the full source image or complete scrolled document");
        require(window.copyVisibleContentToClipboard() &&
                    QApplication::clipboard()->mimeData()->hasImage(),
                "recognition-window copying shares the translation image policy");
        if (overlay) {
            session.displayed->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
            session.displayed->updateTextSelection(ScreenshotOcrTextPosition{0, 10});
            session.displayed->finishTextSelection();
        } else {
            require(editor->document()->revision() == revision &&
                        editor->verticalScrollBar()->value() == scroll &&
                        editor->toPlainText() == result,
                    "full-document rendering preserves live content, revision and scroll");
            auto cursor = editor->textCursor();
            cursor.setPosition(0);
            cursor.setPosition(10, QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
        }
        mime = session.controller->recognitionClipboardMimeData();
        require(mime && !mime->hasImage() && mime->text() == QStringLiteral("Translated"),
                "selected translation text takes precedence over image copying");
        if (overlay) {
            session.displayed->clearTextSelection();
            session.displayed->selectAll();
            mime = session.controller->recognitionClipboardMimeData();
            require(mime && mime->hasText() && !mime->hasImage(), "OCR box selection copies text");
            session.displayed->clearTextSelection();
            session.renderImage = {};
            QApplication::clipboard()->setText(QStringLiteral("keep clipboard"));
            require(!window.copyVisibleContentToClipboard() &&
                        QApplication::clipboard()->text() == QStringLiteral("keep clipboard"),
                    "render failure preserves the clipboard and reports failure");
        }
        require(session.streaming && server.streams.size() == requests,
                "image copy and preference changes leave the streaming job intact");
        server.finish(0);
        waitUntil([&] { return !session.streaming; }, "finish image-copy translation");
        require(settings.setCopyTranslationResultsAsImage(false), "restore image-copy preference");
    }
}

void customModelPageAndScreenshotParity(bool originalImage) {
    TranslationServer server;
    server.holdModels = true;
    snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("44444444-4444-4444-8444-444444444444"),
        QStringLiteral("Shared custom translator"),
        server.url() + QStringLiteral("/v1"),
        {},
        QStringLiteral("provider-model"),
        true};
    const snow_shot::storage::ApiConfigurationSettings apiSettings;
    const auto previousModels = apiSettings.customModels();
    require(apiSettings.setCustomModels({model}), "configure cross-view custom model");
    configureTranslation(originalImage, model.selectionId());
    SnowShotApiClient api(server.url());
    SessionProbe screenshot(&api, 1);
    snow_shot::presentation::TranslationPageController page(
        api, snow_shot::storage::ApplicationStorage::instance().configuration(), QLocale::English,
        nullptr, 0);
    page.activate();
    page.setSourceText(QStringLiteral("source 0"));
    screenshot.controller->beginTextTranslation();
    server.waitForStreams(2);
    require(server.streams[0].body == server.streams[1].body &&
                server.streams[0].body.value(QStringLiteral("model")) == model.model,
            "page and screenshot use identical custom-provider payloads during held discovery");
    for (int index = 0; index < 2; ++index)
        server.delta(index, QStringLiteral("translated"));
    waitUntil(
        [&] {
            return page.resultText() == QStringLiteral("translated") &&
                   screenshot.controller->textDraft() == QStringLiteral("translated");
        },
        "both views display streamed custom translation");
    require(screenshot.controller->sourceTextDraft() == QStringLiteral("source 0"),
            "streamed translation output must not replace the OCR source draft");
    model.name = QStringLiteral("Renamed translator");
    model.supportsVision = false;
    require(apiSettings.setCustomModels({model}), "edit custom model display metadata");
    require(page.translating() && screenshot.streaming,
            "metadata edits leave both translation views running");
    for (int index = 0; index < 2; ++index)
        server.finish(index);
    waitUntil([&] { return !page.translating() && !screenshot.streaming; },
              "both views complete custom translation");
    model.model = QStringLiteral("updated-provider-model");
    require(apiSettings.setCustomModels({model}), "edit shared custom connection");
    QCoreApplication::processEvents();
    require(page.resultText().isEmpty() && !page.errorText().isEmpty() &&
                (originalImage ? screenshot.displayed->lines[0].text == QStringLiteral("source 0")
                               : screenshot.controller->textDraft().isEmpty()) &&
                !screenshot.errors.isEmpty() && !screenshot.streaming && server.streams.size() == 2,
            "connection edits clear both completed views without automatic submission");
    page.retry();
    screenshot.controller->endTextEditing();
    screenshot.controller->beginTextTranslation();
    server.waitForStreams(4);
    require(server.streams[2].body == server.streams[3].body &&
                server.streams[2].body.value(QStringLiteral("model")) == model.model,
            "explicit retries in both views use the updated custom connection");
    require(apiSettings.setCustomModels(previousModels), "restore cross-view custom models");
}

void smartLayoutStreamsParagraphsAndSwitchesModes() {
    configureTranslation();
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    require(settings.setLayoutProcessing(QStringLiteral("smart_merge")), "enable Smart Merge");
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 3);
    session.controller->beginTextTranslation();
    server.waitForStreams(1);
    require(server.streams[0].text == QStringLiteral("source 0 source 1 source 2") &&
                session.displayed->lines.size() == 1 && session.displayed->lines[0].paragraph &&
                session.displayed->lines[0].sourceLineQuads.size() == 3 &&
                session.source->lines.size() == 3 &&
                session.source->lines[0].text == QStringLiteral("source 0"),
            "Smart Merge sends one paragraph with combined geometry and preserves original OCR");
    server.delta(0, QStringLiteral("partial paragraph"));
    waitUntil([&]() { return session.textUpdates == 1; },
              "merged paragraph streams into its block");
    require(settings.setLayoutProcessing(QStringLiteral("original")),
            "switch layout while streaming");
    server.waitForStreams(4);
    waitUntil([&]() { return server.disconnected(0); },
              "layout change cancels stale paragraph request");
    require(session.displayed->lines.size() == 3 &&
                session.displayed->lines[0].text == QStringLiteral("source 0") &&
                server.streams[1].text == QStringLiteral("source 0") &&
                server.streams[3].text == QStringLiteral("source 2"),
            "Original restores OCR unit boundaries and discards old partial text");
    for (int i = 1; i < 4; ++i) {
        server.delta(i, QStringLiteral("translated %1").arg(i));
        server.finish(i);
    }
    waitUntil([&]() { return !session.streaming; }, "original units finish");
    require(settings.setLayoutProcessing(QStringLiteral("smart_merge")),
            "rebuild completed original cache as paragraphs");
    server.waitForStreams(5);
    require(server.streams[4].text == QStringLiteral("source 0 source 1 source 2"),
            "switching back merges source OCR rather than cached translated text");
    server.fail(4);
    waitUntil([&]() { return !session.streaming; }, "merged request fails");
    session.controller->endTextEditing();
    session.controller->beginTextTranslation();
    server.waitForStreams(6);
    require(server.streams[5].text == QStringLiteral("source 0 source 1 source 2"),
            "retry reuses immutable paragraph source");
    server.delta(5, QStringLiteral("finished paragraph"));
    server.finish(5);
    waitUntil([&]() { return !session.streaming; }, "merged retry finishes");
    require(session.displayed->lines[0].text == QStringLiteral("finished paragraph") &&
                session.source->lines[2].text == QStringLiteral("source 2"),
            "merged output and original remain independent");
}

void sourceSelectionSurvivesOriginalImageTranslation() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    ScreenshotRecognitionWindow window({});
    SessionProbe session(&api, 3, QStringLiteral("selection-refresh"), false, nullptr, nullptr,
                         &window);
    for (const int line : {0, 2}) {
        session.source->beginLineBoxSelection(QPointF(-1, line * 25 - 1));
        session.source->updateLineBoxSelection(QRectF(-1, line * 25 - 1, 152, 22));
        session.source->finishLineBoxSelection();
    }
    session.source->beginLineBoxSelection(QPointF(-1, -1));
    session.source->updateLineBoxSelection(QRectF(-1, -1, 152, 47));
    require(
        session.source->lineSelectionPreview(0) == ScreenshotOcrLineSelectionPreview::Deselect &&
            session.source->lineSelectionPreview(1) == ScreenshotOcrLineSelectionPreview::Select,
        "source refresh fixture has a committed sparse selection and an uncommitted preview");
    session.controller->beginTextTranslation();
    require(session.displayed != session.source &&
                session.displayed->selectedText() == QStringLiteral("source 0\nsource 2") &&
                !session.source->lineBoxSelectionActive() &&
                !session.displayed->lineBoxSelectionActive() &&
                !session.displayed->textSelectionActive(),
            "compatible source-to-overlay replacement transfers committed sparse selection only");
    server.waitForStreams(3);
    server.delta(0, QStringLiteral("first translated block"));
    server.delta(2, QStringLiteral("third translated block"));
    waitUntil([&] { return session.textUpdates == 2; }, "selected overlay blocks receive deltas");
    require(session.controller->recognitionClipboardMimeData()->text() ==
                    QStringLiteral("first translated block\nthird translated block") &&
                session.source->selectedText() == QStringLiteral("source 0\nsource 2"),
            "transferred full-line selections grow with translated text independently of source");
    server.delta(1, QStringLiteral("middle translated block"));
    for (int line = 0; line < 3; ++line) {
        server.finish(line);
    }
    waitUntil([&] { return !session.streaming; }, "selected original-image translation finishes");

    const auto freshPresentation = [&] {
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = session.displayed->selection;
        presentation->lines = session.displayed->lines;
        presentation->prepareForRendering();
        return presentation;
    };
    auto canonicalSource = freshPresentation();
    canonicalSource->lines[0].sourceLineQuads = {canonicalSource->lines[0].quad};
    canonicalSource->copyCommittedSelectionFrom(*session.displayed);
    window.setOcrPresentation(canonicalSource);
    auto closedReplacement = std::make_shared<ScreenshotOcrPresentation>();
    closedReplacement->selection = canonicalSource->selection;
    closedReplacement->lines = canonicalSource->lines;
    for (auto& line : closedReplacement->lines) {
        line.quad.push_back(line.quad.constFirst());
        for (auto& sourceQuad : line.sourceLineQuads) {
            sourceQuad.push_back(sourceQuad.constFirst());
        }
    }
    window.setOcrPresentation(closedReplacement);
    require(closedReplacement->selectedText() == session.displayed->selectedText() &&
                closedReplacement->lines[0].quad.size() == 4 &&
                closedReplacement->lines[0].sourceLineQuads[0].size() == 4,
            "fresh closed quads are prepared before compatible refresh selection is compared");
    auto callerSelected = freshPresentation();
    callerSelected->beginTextSelection(ScreenshotOcrTextPosition{1, 0});
    callerSelected->updateTextSelection(
        ScreenshotOcrTextPosition{1, static_cast<int>(callerSelected->lines[1].text.size())});
    callerSelected->finishTextSelection();
    callerSelected->beginLineBoxSelection(QPointF(-1, -1));
    callerSelected->updateLineBoxSelection(QRectF(-1, -1, 152, 47));
    require(callerSelected->lineBoxSelectionActive() && callerSelected->hasTextSelection(),
            "incoming refresh fixture has committed selection and an active box preview");
    window.setOcrPresentation(callerSelected);
    require(callerSelected->selectedText() == QStringLiteral("middle translated block") &&
                !callerSelected->lineBoxSelectionActive() && !window.lineBoxSelectionActive() &&
                QWidget::mouseGrabber() != &window,
            "incoming committed selection takes precedence while its box preview loses ownership");

    callerSelected->beginTextSelection(ScreenshotOcrTextPosition{1, 0});
    callerSelected->updateTextSelection(ScreenshotOcrTextPosition{1, 6});
    auto characterReplacement = freshPresentation();
    window.setOcrPresentation(characterReplacement);
    require(!callerSelected->textSelectionActive() &&
                !characterReplacement->textSelectionActive() &&
                characterReplacement->selectedText() == QStringLiteral("middle"),
            "a compatible replacement commits the outgoing character range without its drag");
    window.setOcrPresentation(callerSelected);
    require(!callerSelected->textSelectionActive() &&
                callerSelected->selectedText() == QStringLiteral("middle"),
            "reinstalling a retired source presentation cannot resume its character gesture");
    auto incomingCharacterSelection = freshPresentation();
    incomingCharacterSelection->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
    incomingCharacterSelection->updateTextSelection(ScreenshotOcrTextPosition{0, 5});
    window.setOcrPresentation(incomingCharacterSelection);
    require(!incomingCharacterSelection->textSelectionActive() &&
                incomingCharacterSelection->selectedText() == QStringLiteral("first") &&
                QWidget::mouseGrabber() != &window,
            "installing an incoming active character selection preserves only its committed range");
    incomingCharacterSelection->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
    incomingCharacterSelection->updateTextSelection(ScreenshotOcrTextPosition{0, 5});
    window.setOcrPresentation(incomingCharacterSelection);
    require(!incomingCharacterSelection->textSelectionActive() &&
                incomingCharacterSelection->selectedText() == QStringLiteral("first"),
            "reinstalling the same model also ends character selection ownership");

    const auto incompatibleReplacement = [&](const auto& change) {
        window.setOcrPresentation(session.displayed);
        auto replacement = freshPresentation();
        change(*replacement);
        replacement->prepareForRendering();
        window.setOcrPresentation(replacement);
        require(!replacement->hasTextSelection(),
                "incompatible OCR replacements do not inherit committed selection");
    };
    incompatibleReplacement([](auto& value) { value.selection.translate(1, 0); });
    incompatibleReplacement([](auto& value) { value.lines[0].text.append(QLatin1Char('!')); });
    incompatibleReplacement([](auto& value) { value.lines[0].quad.translate(1, 0); });
    incompatibleReplacement(
        [](auto& value) { value.lines[0].sourceLineQuads.push_back(value.lines[0].quad); });
    incompatibleReplacement(
        [](auto& value) { value.lines[0].direction = ScreenshotOcrTextDirection::Vertical; });
    incompatibleReplacement([](auto& value) { value.lines[0].paragraph = true; });

    window.setOcrPresentation(session.displayed);
    auto hiddenReplacement = freshPresentation();
    window.setOcrPresentation(hiddenReplacement,
                              ScreenshotOcrTextLayer::RenderingMode::SelectionOnly);
    require(!hiddenReplacement->hasTextSelection(),
            "rendering-mode changes do not inherit visible OCR selection");
    window.setOcrPresentation(session.displayed,
                              ScreenshotOcrTextLayer::RenderingMode::SelectionOnly);
    auto visibleReplacement = freshPresentation();
    window.setOcrPresentation(visibleReplacement);
    require(!visibleReplacement->hasTextSelection(),
            "returning from SelectionOnly does not inherit its selection");
    auto clearedSource = freshPresentation();
    window.setOcrPresentation(clearedSource);
    clearedSource->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
    clearedSource->updateTextSelection(ScreenshotOcrTextPosition{0, 5});
    window.clearOcrPresentation();
    require(!clearedSource->textSelectionActive() &&
                clearedSource->selectedText() == QStringLiteral("first"),
            "explicit clearing retires a retained model's character gesture without erasing range");
    auto clearedReplacement = freshPresentation();
    window.setOcrPresentation(clearedReplacement);
    require(!clearedReplacement->hasTextSelection(),
            "explicit content clearing remains a selection-transfer boundary");
    window.setOcrPresentation(clearedSource);
    require(!clearedSource->textSelectionActive() &&
                clearedSource->selectedText() == QStringLiteral("first"),
            "reinstalling an explicitly cleared model cannot revive its retired character gesture");
}

void streamsIndividualBoxesAndKeepsOriginals() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 6);
    session.controller->beginTextTranslation();
    require(session.streaming && session.overlayState && !session.controller->editing(),
            "overlay mode should be busy during model discovery without entering the editor");
    server.waitForStreams(6);
    require(server.modelRequests == 1, "resolve translation models once per image run");
    for (int index = 0; index < 6; ++index) {
        require(server.streams[index].text == session.source->lines[index].text,
                "each request must carry exactly its source OCR box");
    }
    const int backgrounds = session.backgrounds;
    server.delta(2, QStringLiteral("third"));
    server.delta(0, QStringLiteral("first"));
    waitUntil([&]() { return session.textUpdates == 2; },
              "display interleaved deltas before completion");
    require(session.displayed->lines[2].text == QStringLiteral("third") &&
                session.displayed->lines[1].text == QStringLiteral("source 1") &&
                session.displayed->lines[5].text == QStringLiteral("source 5"),
            "streaming boxes should retain independent display content");
    const auto snapshot = session.controller->recognitionResultsSnapshot();
    require(snapshot.text.has_value() &&
                snapshot.text->presentation->lines[2].text == QStringLiteral("source 2"),
            "pin snapshots must retain source OCR independently of displayed translation");
    require(snapshot.translatedText != nullptr && snapshot.translatedText != session.displayed &&
                snapshot.translatedText->lines[2].text == QStringLiteral("third") &&
                snapshot.translatedText->lines[1].text == QStringLiteral("source 1"),
            "a pin snapshot must freeze partial translation and untouched source boxes");
    session.displayed->beginTextSelection(ScreenshotOcrTextPosition{0, 1});
    session.displayed->updateTextSelection(ScreenshotOcrTextPosition{0, 4});
    session.displayed->finishTextSelection();
    require(session.controller->recognitionClipboardMimeData()->text() == QStringLiteral("irs"),
            "copy should honor the translated text selection");
    session.displayed->beginLineBoxSelection(QPointF(-1, -1));
    session.displayed->updateLineBoxSelection(QRectF(-1, -1, 152, 22));
    session.displayed->finishLineBoxSelection();
    session.displayed->beginLineBoxSelection(QPointF(-1, 49));
    session.displayed->updateLineBoxSelection(QRectF(-1, 49, 152, 22));
    session.displayed->finishLineBoxSelection();
    require(session.controller->recognitionClipboardMimeData()->text() ==
                QStringLiteral("first\nthird"),
            "box selection copies independent translated blocks in OCR reading order");
    session.displayed->beginLineBoxSelection(QPointF(-1, -1));
    session.displayed->updateLineBoxSelection(QRectF(-1, -1, 152, 72));
    require(session.displayed->lineSelectionPreview(0) ==
                    ScreenshotOcrLineSelectionPreview::Deselect &&
                session.displayed->lineSelectionPreview(1) ==
                    ScreenshotOcrLineSelectionPreview::Select &&
                session.displayed->lineSelectionPreview(2) ==
                    ScreenshotOcrLineSelectionPreview::Deselect,
            "streaming translations expose mixed add/remove previews from the press baseline");
    server.delta(2, QStringLiteral(" box"));
    server.finish(2);
    waitUntil([&] { return session.displayed->lines[2].text == QStringLiteral("third box"); },
              "the streamed delta reaches the selected translated box");
    require(snapshot.translatedText->lines[2].text == QStringLiteral("third"),
            "later translation deltas must not modify a previously captured pin snapshot");
    require(session.displayed->selectedText() == QStringLiteral("first\nthird box") &&
                session.displayed->lineSelectionPreview(2) ==
                    ScreenshotOcrLineSelectionPreview::Deselect &&
                session.backgrounds == backgrounds,
            "full-block selections grow with streaming without reversing the preview or filtering");
    const auto selectedSnapshot = session.controller->recognitionResultsSnapshot();
    require(!selectedSnapshot.translatedText->hasTextSelection() &&
                !selectedSnapshot.translatedText->lineBoxSelectionActive() &&
                !session.source->hasTextSelection(),
            "pin snapshots and source OCR exclude live selection and preview state");
    session.displayed->cancelLineBoxSelection();
    session.displayed->beginTextSelection(ScreenshotOcrTextPosition{0, 1});
    session.displayed->updateTextSelection(ScreenshotOcrTextPosition{0, 4});
    session.displayed->finishTextSelection();
    require(session.displayed->selectedText() == QStringLiteral("irs") &&
                session.backgrounds == backgrounds,
            "stream updates should preserve unrelated selection and the OCR background");
    server.finish(0);
    for (const int index : {1, 3, 4, 5}) {
        server.delta(index, QStringLiteral("result %1").arg(index));
        server.finish(index);
    }
    waitUntil([&]() { return !session.streaming; }, "finish all box translations");
    session.displayed->clearTextSelection();
    require(session.controller->recognitionClipboardMimeData()->text() ==
                QStringLiteral("first\nresult 1\nthird box\nresult 3\nresult 4\nresult 5"),
            "whole-image copy should preserve source OCR order");
    require(session.controller->originalText().startsWith(QStringLiteral("source 0")) &&
                session.controller->cachedRecognitionResults().text->presentation ==
                    session.source &&
                session.source->lines[0].text == QStringLiteral("source 0"),
            "original and persisted recognition data must remain untouched");
    session.controller->applyTextFormatting(QStringLiteral("single_line"));
    session.controller->setTextDraft(QStringLiteral("overwrite"));
    require(session.displayed->lines[0].text == QStringLiteral("first"),
            "editor commands must not change overlay translation");
    session.controller->endTextEditing();
    require(session.displayed == session.source, "leaving translation restores source OCR");
    const auto sourceSnapshot = session.controller->recognitionResultsSnapshot();
    require(sourceSnapshot.text->presentation->lines[0].text == QStringLiteral("source 0") &&
                sourceSnapshot.translatedText->lines[0].text == QStringLiteral("first"),
            "leaving translation must retain separate source and translation snapshots");
    session.controller->beginTextTranslation();
    require(!session.streaming && server.streams.size() == 6, "reuse completed image translation");
    session.controller->endTextEditing();
    session.controller->beginTextEditing();
    require(session.controller->editing() &&
                session.controller->textDraft().contains(QStringLiteral("source 0")),
            "Edit should open source OCR text");
}

void fourBoxesAndOwnerClosure() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 4);
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    for (int index = 3; index >= 0; --index) {
        server.delta(index, QStringLiteral("four %1").arg(index));
        server.finish(index);
    }
    waitUntil([&]() { return !session.streaming; }, "all four boxes should complete");
    require(session.controller->textDraft() == QStringLiteral("four 0\nfour 1\nfour 2\nfour 3"),
            "four concurrent results must preserve OCR order after reverse completion");
    SessionProbe closing(&api, kBuiltInTranslationConcurrency + 2);
    closing.controller->beginTextTranslation();
    const int requests = 4 + kBuiltInTranslationConcurrency;
    server.waitForStreams(requests);
    closing.controller.reset();
    for (int index = 4; index < requests; ++index) {
        waitUntil([&]() { return server.disconnected(index); }, "owner destruction closes streams");
    }
    require(server.streams.size() == requests, "owner destruction must discard queued boxes");
}

void partialFailuresRetryOnlyFailedBoxes() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 4);
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    server.delta(0, QStringLiteral("partial"));
    server.fail(0);
    waitUntil([&]() { return server.disconnected(0); },
              "terminal SSE error must close its HTTP reply");
    server.fail(1);
    server.finish(2);
    server.delta(3, QStringLiteral("complete"));
    server.finish(3);
    waitUntil([&]() { return !session.streaming; }, "partial failure run should settle");
    require(session.errors.size() == 1, "partial failure should report exactly one error summary");
    require(session.errors.first().contains(QStringLiteral("test failure")) ||
                session.errors.first().contains(QStringLiteral("The model returned no content")),
            "overlay failure summary includes the underlying reason");
    require(session.displayed->lines[0].text == QStringLiteral("partial"),
            "the first failed box should keep its partial output");
    require(session.displayed->lines[1].text == QStringLiteral("source 1"),
            "failure before a delta should keep the second source box");
    require(session.displayed->lines[2].text == QStringLiteral("source 2"),
            "empty completion should keep the third source box");
    session.controller->endTextEditing();
    session.controller->beginTextTranslation();
    server.waitForStreams(7);
    require(session.displayed->lines[0].text == QStringLiteral("partial") &&
                session.displayed->lines[3].text == QStringLiteral("complete"),
            "retry should retain previous partial and completed output until new deltas");
    for (int index = 4; index < 7; ++index) {
        require(server.streams[index].text == QStringLiteral("source %1").arg(index - 4),
                "retry only failed boxes using source text");
        server.delta(index, QStringLiteral("retry %1").arg(index - 4));
        server.finish(index);
    }
    waitUntil([&]() { return !session.streaming; }, "retry should settle");
    require(session.displayed->lines[0].text == QStringLiteral("retry 0") &&
                session.errors.size() == 1,
            "retry replaces partial text and does not repeat a resolved failure summary");
}

void rateLimitingStopsQueuedBoxesAndRetryPreservesCompletedResults() {
    configureTranslation();
    TranslationServer server;
    server.holdStreamHeaders = true;
    SnowShotApiClient api(server.url());
    constexpr int boxes = kBuiltInTranslationConcurrency + 4;
    constexpr int accepted = kBuiltInTranslationConcurrency + 1;
    SessionProbe session(&api, boxes);
    session.controller->beginTextTranslation();
    server.waitForStreams(kBuiltInTranslationConcurrency);
    server.delta(0, QStringLiteral("completed before limit"));
    server.finish(0);
    server.waitForStreams(accepted);
    bool rateLimitHandled = false;
    QObject completionObserver;
    for (auto* reply : api.findChildren<QNetworkReply*>()) {
        QObject::connect(reply, &QNetworkReply::finished, &completionObserver, [&, reply] {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429)
                rateLimitHandled = true;
        });
    }
    server.rateLimit(1);
    waitUntil([&] { return rateLimitHandled; }, "client must observe the rate-limit response");
    for (int index = 2; index < accepted; ++index) {
        server.delta(index, QStringLiteral("in flight %1").arg(index));
        server.finish(index);
    }
    waitUntil([&] { return !session.streaming || server.streams.size() > accepted; },
              "rate-limited batch should stop after its in-flight work finishes");
    require(server.streams.size() == accepted,
            "HTTP 429 must stop dispatch of queued translation boxes");
    require(!session.streaming && session.errors.size() == 1,
            "rate-limited batch should leave busy state and report one failure");
    require(session.displayed->lines[0].text == QStringLiteral("completed before limit") &&
                session.displayed->lines[accepted - 1].text ==
                    QStringLiteral("in flight %1").arg(accepted - 1) &&
                session.displayed->lines[1].text == QStringLiteral("source 1") &&
                session.displayed->lines[boxes - 1].text ==
                    QStringLiteral("source %1").arg(boxes - 1),
            "rate limiting must retain translations and the original text of unsent boxes");
    session.controller->endTextEditing();
    session.controller->beginTextTranslation();
    QStringList expectedRetry{QStringLiteral("source 1")};
    for (int index = accepted; index < boxes; ++index)
        expectedRetry.append(QStringLiteral("source %1").arg(index));
    expectedRetry.sort();
    const int requests = accepted + static_cast<int>(expectedRetry.size());
    server.waitForStreams(requests);
    QStringList retried;
    for (int index = accepted; index < requests; ++index) {
        retried.append(server.streams[index].text);
        server.delta(index, QStringLiteral("retried %1").arg(server.streams[index].text));
        server.finish(index);
    }
    require(retried == expectedRetry,
            "explicit retry must send only the rejected and previously unsent boxes");
    waitUntil([&] { return !session.streaming; }, "explicit rate-limit retry should finish");
    require(session.errors.size() == 1 &&
                session.displayed->lines[0].text == QStringLiteral("completed before limit") &&
                session.displayed->lines[boxes - 1].text ==
                    QStringLiteral("retried source %1").arg(boxes - 1),
            "successful retry must retain earlier results and clear the failed batch state");
}

void backgroundWorkAndIndependentSessions() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe first(&api, 3, QStringLiteral("first"));
    SessionProbe second(&api, 3, QStringLiteral("second"));
    first.controller->beginTextTranslation();
    server.waitForStreams(3);
    second.controller->beginTextTranslation();
    server.waitForStreams(6);
    first.controller->endTextEditing();
    require(first.streaming && first.displayed == first.source && !first.overlayState,
            "returning to OCR must preserve translation busy state and source display");
    first.controller->deactivate();
    require(first.streaming, "another tool must not hide background translation progress");
    const int previousUpdates = first.textUpdates;
    for (int index = 0; index < 6; ++index) {
        server.delta(index, QStringLiteral("translated %1").arg(index));
        server.finish(index);
    }
    waitUntil([&]() { return !second.streaming; }, "independent image queues should finish");
    require(first.textUpdates == previousUpdates && !first.overlayState,
            "background completion must not update the inactive view or toolbar");
    first.controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    first.controller->beginTextTranslation();
    waitUntil([&]() { return !first.streaming; }, "cached background result should be complete");
    require(first.displayed->lines[0].text == QStringLiteral("translated 0") &&
                server.streams.size() == 6,
            "returning to translation should display cached background results");
}

void backgroundFailuresAreReportedOnReturn() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 1);
    session.controller->beginTextTranslation();
    server.waitForStreams(1);
    session.controller->endTextEditing();
    server.fail(0);
    waitUntil([&]() { return !session.streaming; }, "background failure should settle");
    require(session.errors.isEmpty() && session.displayed == session.source,
            "background failure must leave OCR visible without an error popup");
    session.controller->beginTextTranslation();
    server.waitForStreams(2);
    require(session.errors.size() == 1, "returning should summarize the failed background run");
    server.delta(1, QStringLiteral("retried"));
    server.finish(1);
    waitUntil([&]() { return !session.streaming; }, "retry background failure");
    require(session.errors.size() == 1 &&
                session.displayed->lines[0].text == QStringLiteral("retried"),
            "a successful retry must not repeat the previous failure summary");
}

void translationSettingsSwitchKeepsReadableSize() {
    configureTranslation();
    QWidget owner;
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 1, QStringLiteral("image"), false, nullptr, &owner);
    session.controller->openTranslationSettings();
    auto* modal = session.controller->findChild<adqt::widgets::AdModal*>(
        QStringLiteral("screenshotTranslationSettingsModal"));
    require(modal != nullptr, "open translation settings for switch sizing");
    auto* toggle = modal->contentWidget()->findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("screenshotTranslationOriginalImage"));
    require(toggle != nullptr, "find original image translation switch for sizing");
    adqt::widgets::AdSwitch standardSwitch;
    for (const bool checked : {false, true}) {
        toggle->setChecked(checked);
        standardSwitch.setChecked(checked);
        QCoreApplication::processEvents();
        require(toggle->sizeHint() == standardSwitch.sizeHint(),
                "translation switch uses the same default dimensions as the model editor");
        require(toggle->width() >= toggle->sizeHint().width() &&
                    toggle->height() >= toggle->sizeHint().height(),
                "translation settings layout must not shrink the switch below its size hint");
    }
    modal->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void confirmingTranslationDisplayModeRestartsTranslation() {
    configureTranslation();
    QWidget owner;
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 2, QStringLiteral("image"), false, nullptr, &owner);
    session.controller->beginTextTranslation();
    server.waitForStreams(2);

    const auto changeMode = [&](bool enabled, bool accept) {
        session.controller->openTranslationSettings();
        auto* modal = session.controller->findChild<adqt::widgets::AdModal*>(
            QStringLiteral("screenshotTranslationSettingsModal"));
        require(modal != nullptr, "open translation settings for active translation");
        auto* toggle = modal->contentWidget()->findChild<adqt::widgets::AdSwitch*>(
            QStringLiteral("screenshotTranslationOriginalImage"));
        require(toggle != nullptr, "find original image translation toggle");
        toggle->setChecked(enabled);
        modal->closeRequested(accept ? adqt::widgets::AdModal::CloseReason::OkAction
                                     : adqt::widgets::AdModal::CloseReason::CancelAction);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };

    changeMode(false, false);
    require(session.overlayState && server.streams.size() == 2,
            "Cancel must preserve the active translation and its requests");
    changeMode(false, true);
    require(session.controller->translating() && session.controller->editing() &&
                !session.overlayState,
            "confirming disabled original image translation must immediately show the editor");
    server.waitForStreams(3);
    require(server.streams[2].text == QStringLiteral("source 0\nsource 1"),
            "confirming editor mode must retranslate the full source text");
    for (int index = 0; index < 2; ++index) {
        waitUntil([&]() { return server.disconnected(index); },
                  "confirming a mode change must cancel old overlay streams");
    }
    server.delta(2, QStringLiteral("whole translation"));
    server.finish(2);
    waitUntil([&]() { return !session.streaming; }, "complete editor translation");
    changeMode(true, true);
    require(session.controller->translating() && !session.controller->editing() &&
                session.overlayState,
            "confirming enabled original image translation must immediately show the overlay");
    server.waitForStreams(5);
    for (int index = 3; index < 5; ++index) {
        server.delta(index, QStringLiteral("new translation %1").arg(index));
        server.finish(index);
    }
    waitUntil([&]() { return !session.streaming; }, "complete refreshed overlay translation");
    require(session.displayed->lines[0].text == QStringLiteral("new translation 3"),
            "the refreshed overlay must display the new translation");
    changeMode(true, true);
    require(!session.streaming && server.streams.size() == 5,
            "confirming unchanged settings must retain the completed translation");
}

void invalidationAndModeChangesCancelOldWork() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 4);
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    require(snow_shot::storage::ScreenshotTranslationSettings().setOriginalImageTranslationEnabled(
                false),
            "disable original image translation");
    require(session.overlayState,
            "changing the toggle must not replace the currently visible view");
    session.controller->endTextEditing();
    session.controller->beginTextTranslation();
    server.waitForStreams(5);
    require(session.controller->editing() && !session.overlayState &&
                server.streams.last().text ==
                    QStringLiteral("source 0\nsource 1\nsource 2\nsource 3"),
            "disabled mode must use one whole-text editor request");
    for (int index = 0; index < 4; ++index) {
        waitUntil([&]() { return server.disconnected(index); },
                  "mode switch should cancel old streams");
    }
    server.delta(4, QStringLiteral("whole text"));
    server.finish(4);
    waitUntil([&]() { return !session.streaming; }, "legacy translation should complete");
    require(session.controller->textDraft() == QStringLiteral("whole text"),
            "retain legacy editor output");
    session.controller->endTextEditing();
    configureTranslation();
    session.controller->beginTextTranslation();
    server.waitForStreams(9);
    session.controller->invalidate();
    for (int index = 5; index < 9; ++index) {
        waitUntil([&]() { return server.disconnected(index); },
                  "target invalidation should cancel all streams");
    }
    require(server.streams.size() == 9 && !session.controller->hasTarget(),
            "invalidated translation must never dispatch more requests");
}

void quickPrimaryTargetRestartsOriginalBlocks() {
    configureTranslation();
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    auto configuration = settings.configuration();
    configuration.secondaryTargetLanguage = QStringLiteral("ja");
    require(settings.setConfiguration(configuration), "save a secondary target for quick changes");
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 2);
    session.controller->beginTextTranslation();
    server.waitForStreams(2);
    server.delta(0, QStringLiteral("old partial"));
    waitUntil([&] { return session.textUpdates == 1; }, "show the old partial result");
    server.delta(1, QStringLiteral("queued stale output"));
    session.controller->applyTextTargetLanguage(QStringLiteral("zh-Hant"));
    server.waitForStreams(4);
    require(settings.configuration().targetLanguage == QStringLiteral("zh-Hant") &&
                settings.configuration().secondaryTargetLanguage == QStringLiteral("ja") &&
                session.primaryTarget == QStringLiteral("zh-Hant") &&
                session.displayed->lines[0].text == QStringLiteral("source 0") &&
                session.displayed->lines[1].text == QStringLiteral("source 1"),
            "quick primary changes preserve secondary and discard old output");
    for (int index = 0; index < 2; ++index) {
        waitUntil([&] { return server.disconnected(index); }, "quick changes cancel stale streams");
        const auto& request = server.streams[index + 2];
        const auto prompt = request.body.value(QStringLiteral("messages"))
                                .toArray()
                                .first()
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
        require(request.text == session.source->lines[index].text &&
                    prompt.contains(QStringLiteral("Target language: zh-Hant")) &&
                    !prompt.contains(QStringLiteral("Secondary target language")),
                "retranslation uses each original block and the latest concrete target");
    }
    session.controller->applyTextTargetLanguage(QStringLiteral("auto"));
    require(settings.configuration().targetLanguage == QStringLiteral("zh-Hant") &&
                session.primaryTarget == QStringLiteral("zh-Hant") && !session.errors.isEmpty() &&
                server.streams.size() == 4,
            "rejected quick-target writes restore the accepted selection without restarting "
            "translation");
}

void mixedOriginalBoxesShareOneDetectedTarget() {
    configureTranslation();
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    auto configuration = settings.configuration();
    configuration.sourceLanguage = QStringLiteral("zh-Hans");
    configuration.targetLanguage = QStringLiteral("en");
    configuration.secondaryTargetLanguage = QStringLiteral("ja");
    require(settings.setConfiguration(configuration), "configure original-image target routing");
    const QStringList texts{QStringLiteral("Bonjour"),
                            QStringLiteral("The complete source contains mostly English text, "
                                           "so both original image boxes share one target.")};
    TranslationServer server;
    SnowShotApiClient api(server.url());
    int detectionCalls = 0;
    snow_shot::translation::TranslationService::forClient(
        api, snow_shot::storage::ApplicationStorage::instance().configuration(), QLocale::English,
        [&](const QString& input) {
            ++detectionCalls;
            require(input == texts.join(u'\n'),
                    "original image boxes are detected together using their complete source");
            return std::optional<snow_shot::translation::DetectedTranslationLanguage>{
                {QStringLiteral("eng"), 0.8}};
        });
    SessionProbe session(&api, 2, QStringLiteral("complete-input"), false, nullptr, nullptr,
                         nullptr, texts);
    session.controller->beginTextTranslation();
    server.waitForStreams(2);
    require(detectionCalls == 1, "original image translation detects all boxes in one call");
    QStringList transmittedTexts;
    for (const auto& stream : server.streams) {
        const auto prompt = stream.body.value(QStringLiteral("messages"))
                                .toArray()
                                .first()
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
        require(prompt.contains(QStringLiteral("Source language: zh-Hans\nTarget language: ja\n")),
                "all original boxes use the detected target while retaining the source hint");
        transmittedTexts.append(stream.text);
    }
    transmittedTexts.sort();
    auto originalTexts = texts;
    originalTexts.sort();
    require(transmittedTexts == originalTexts,
            "original-image translation keeps each complete original block intact");
    session.controller->endTextEditing();
}

void languageChangesRestartAndProviderDestructionCancels() {
    configureTranslation();
    TranslationServer server;
    auto api = std::make_unique<SnowShotApiClient>(server.url());
    SessionProbe session(api.get(), 4);
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    server.delta(0, QStringLiteral("old"));
    waitUntil([&]() { return session.textUpdates == 1; }, "old run should have partial output");
    require(snow_shot::storage::ScreenshotTranslationSettings().setConfiguration(
                {QStringLiteral("ja"), QStringLiteral("en"), QStringLiteral("qwen"),
                 QStringLiteral("original")}),
            "change translation model and both languages together");
    server.waitForStreams(8);
    require(session.displayed->lines[0].text == QStringLiteral("source 0"),
            "changing languages invalidates old partial output");
    const auto options =
        server.streams[4].body.value(QStringLiteral("translation_options")).toObject();
    require(server.streams[4].body.value(QStringLiteral("model")) == QStringLiteral("qwen") &&
                options.value(QStringLiteral("source_lang")) == QStringLiteral("ja") &&
                options.value(QStringLiteral("target_lang")) == QStringLiteral("en"),
            "a settings batch must restart once using the new model and both languages");
    for (int index = 0; index < 4; ++index) {
        waitUntil([&]() { return server.disconnected(index); },
                  "language change cancels old requests");
    }
    api.reset();
    waitUntil([&]() { return !session.streaming; }, "provider destruction clears busy state");
    require(session.errors.size() == 1 && server.streams.size() == 8,
            "provider loss should report one failure and cancel active work");
}

void emptyFormattedAndPreparationFailures() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    {
        SessionProbe empty(&api, 0);
        empty.controller->beginTextTranslation();
        require(!empty.streaming && server.modelRequests == 0 && server.streams.isEmpty(),
                "empty OCR must not start model discovery or translation");
    }
    {
        SessionProbe blank(&api, 1);
        blank.source->setLineText(0, QStringLiteral(" \t "));
        blank.controller->beginTextTranslation();
        require(!blank.streaming && server.streams.isEmpty(), "blank boxes should be skipped");
    }
    {
        SessionProbe formatted(&api, 0, QStringLiteral("formatted"), true);
        formatted.controller->beginTextTranslation();
        server.waitForStreams(1);
        require(formatted.controller->editing() &&
                    server.streams[0].text == QStringLiteral("formatted source"),
                "formatted text without OCR boxes should retain the editor workflow");
    }
    {
        SessionProbe missing(nullptr, 1);
        missing.controller->beginTextTranslation();
        require(!missing.streaming && missing.errors.size() == 1,
                "missing API must fail without a stuck loading state");
    }
    {
        SnowShotApiClient invalidApi(QStringLiteral(""));
        SessionProbe unsupported(&invalidApi, 5);
        unsupported.controller->beginTextTranslation();
        require(!unsupported.streaming && unsupported.errors.size() == 1 &&
                    server.streams.size() == 1,
                "request-preparation failures must settle without starting HTTP requests");
    }
    configureTranslation();
    TranslationServer unavailable;
    unavailable.rejectModels = true;
    SnowShotApiClient unavailableApi(unavailable.url());
    SessionProbe failedModels(&unavailableApi, 2);
    failedModels.controller->beginTextTranslation();
    waitUntil([&]() { return !failedModels.streaming; },
              "model discovery failure must clear busy state");
    require(failedModels.errors.size() == 1 && unavailable.streams.isEmpty(),
            "model discovery failure should not launch unit requests");
}

class SnapshotOcr final : public ScreenshotOcrRecognitionPort {
  public:
    RequestToken recognize(ScreenshotOcrRequest, QObject*, Completion) override {
        return 0;
    }
    RequestToken render(ScreenshotOcrRequest request, QObject*, Completion completion) override {
        snapshot = std::move(request.presentation);
        pending = std::move(completion);
        return ++token;
    }
    void cancel(RequestToken) override {}
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return false;
    }
    std::shared_ptr<ScreenshotOcrPresentation> snapshot;
    Completion pending;
    RequestToken token = 0;
};

void renderSnapshotsAndLateCallbacksStayIsolated() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    auto ocr = std::make_unique<SnapshotOcr>();
    SessionProbe session(&api, 4, QStringLiteral("snapshot"), false, ocr.get());
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    const auto snapshot = ocr->snapshot;
    const auto lateRender = ocr->pending;
    require(snapshot != nullptr && snapshot != session.displayed,
            "background render should own an immutable presentation snapshot");
    server.delta(0, QStringLiteral("streamed"));
    waitUntil([&]() { return session.textUpdates == 1; },
              "stream a box while background render waits");
    require(snapshot->lines[0].text == QStringLiteral("source 0"),
            "streaming must not mutate a worker's snapshot");
    ocr.reset();
    waitUntil([&]() { return !session.streaming; }, "OCR provider loss should cancel translation");
    for (int index = 0; index < 4; ++index) {
        waitUntil([&]() { return server.disconnected(index); }, "OCR provider loss closes streams");
    }
    session.controller->invalidate();
    ScreenshotOcrRecognitionResult obsolete;
    obsolete.presentation = snapshot;
    obsolete.filteredImage = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    lateRender(std::move(obsolete));
    require(!session.controller->hasTarget() && server.streams.size() == 4,
            "late render callbacks must not revive an invalidated translation queue");
    TranslationServer held;
    held.holdModels = true;
    SnowShotApiClient heldApi(held.url());
    SessionProbe discovering(&heldApi, 1);
    discovering.controller->beginTextTranslation();
    waitUntil([&]() { return held.modelRequests == 1; }, "begin model discovery");
    discovering.controller->invalidate();
    require(!discovering.streaming && held.streams.isEmpty(),
            "invalidation during model discovery must clear busy state without unit requests");
}

void fragmentedUnicodeStreamsUpdateOnlyCompleteEvents() {
    configureTranslation(true, QStringLiteral("qwen"));
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 1);
    session.controller->beginTextTranslation();
    server.waitForStreams(1);
    require(server.streams[0].body.value(QStringLiteral("incremental_output")).toBool() &&
                server.streams[0]
                        .body.value(QStringLiteral("translation_options"))
                        .toObject()
                        .value(QStringLiteral("target_lang"))
                        .toString() == QStringLiteral("zh"),
            "per-box translation must preserve Qwen streaming and language mapping");
    const QString translated = QString::fromUcs4(U"\u4f60\u597d\U0001f642");
    const QByteArray event = TranslationServer::deltaEvent(translated);
    const qsizetype split = event.indexOf(translated.toUtf8()) + 1;
    server.send(0, event.left(split));
    QCoreApplication::processEvents();
    require(session.textUpdates == 0, "an incomplete UTF-8 SSE event must not update the overlay");
    server.send(0, event.mid(split));
    waitUntil([&]() { return session.textUpdates == 1; },
              "fragmented Unicode delta should reach overlay");
    require(session.displayed->lines[0].text == translated && session.streaming,
            "complete Unicode text should display before the done marker");
    server.finish(0);
    waitUntil([&]() { return !session.streaming; }, "single-box translation should finish");
}
} // namespace

void runOriginalImageTranslationTests() {
    translationImageCopyHonorsSelectionAndSnapshots();
    mixedOriginalBoxesShareOneDetectedTarget();
    quickPrimaryTargetRestartsOriginalBlocks();
    customModelPageAndScreenshotParity(false);
    customModelPageAndScreenshotParity(true);
    translationSettingsSwitchKeepsReadableSize();
    confirmingTranslationDisplayModeRestartsTranslation();
    smartLayoutStreamsParagraphsAndSwitchesModes();
    sourceSelectionSurvivesOriginalImageTranslation();
    streamsIndividualBoxesAndKeepsOriginals();
    fourBoxesAndOwnerClosure();
    partialFailuresRetryOnlyFailedBoxes();
    rateLimitingStopsQueuedBoxesAndRetryPreservesCompletedResults();
    backgroundWorkAndIndependentSessions();
    backgroundFailuresAreReportedOnReturn();
    invalidationAndModeChangesCancelOldWork();
    languageChangesRestartAndProviderDestructionCancels();
    emptyFormattedAndPreparationFailures();
    renderSnapshotsAndLateCallbacksStayIsolated();
    fragmentedUnicodeStreamsUpdateOnlyCompleteEvents();
}

#if defined(SNOW_SHOT_TEST_PINNED_TRANSLATION)
void runPinnedOriginalImageTranslationTests() {
    configureTranslation();
    {
        const snow_shot::storage::ScreenshotTranslationSettings settings;
        require(settings.setLayoutProcessing(QStringLiteral("smart_merge")), "enable Smart Merge");
        TranslationServer mergedServer;
        SnowShotApiClient mergedApi(mergedServer.url());
        SessionProbe mergedSeed(&mergedApi, 3);
        mergedSeed.controller->beginTextTranslation();
        mergedServer.waitForStreams(1);
        mergedServer.delta(0, QStringLiteral("translated paragraph across multiple lines"));
        mergedServer.finish(0);
        waitUntil([&]() { return !mergedSeed.streaming; }, "finish merged translation for pinning");
        const auto results = mergedSeed.controller->recognitionResultsSnapshot();
        mergedSeed.controller.reset();
        require(results.translatedText != nullptr && results.translatedText->lines.size() == 1,
                "pin fixture must merge three OCR lines into one translated paragraph");
        const auto& expected = results.translatedText->lines.front();
        ScreenshotPinnedWindow::Config mergedConfig;
        mergedConfig.canvasSourceRect = mergedSeed.source->selection;
        mergedConfig.screen = QGuiApplication::primaryScreen();
        require(mergedConfig.screen != nullptr, "merged pin requires a test screen");
        mergedConfig.nativeGeometry =
            QRect(ScreenshotGeometryMapper::physicalRectForScreen(*mergedConfig.screen).topLeft() +
                      QPoint(40, 40),
                  mergedSeed.source->selection.size());
        QImage image(mergedSeed.source->selection.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        mergedConfig.imageSource =
            ScreenshotImageSource::fromImage(image, mergedConfig.canvasSourceRect);
        mergedConfig.automaticTextRecognition = false;
        mergedConfig.recognitionVisible = true;
        mergedConfig.translationVisible = true;
        mergedConfig.recognitionResults = results;
        // Captured translations must survive even if the global layout setting has changed.
        configureTranslation(false);
        for (const bool restore : {false, true}) {
            mergedConfig.restorePersistentState = restore;
            auto* window = new ScreenshotPinnedWindow;
            QPointer<ScreenshotPinnedWindow> guard(window);
            require(window->present(mergedConfig), "present merged translation pin");
            auto* session = window->findChild<ScreenshotRecognitionSessionController*>();
            require(session != nullptr, "merged pin owns a recognition session");
            waitUntil([&]() { return session->hasTextResult(); }, "load merged pin OCR");
            require(session->originalImageTranslationActive(),
                    "pinning and restoring must preserve the merged translation overlay");
            const auto snapshot = session->recognitionResultsSnapshot();
            require(snapshot.translatedText != nullptr &&
                        snapshot.translatedText->lines.size() == 1 &&
                        snapshot.text->presentation->lines.size() == 3,
                    "merged pins preserve separate source lines and translated paragraphs");
            const auto& actual = snapshot.translatedText->lines.front();
            require(actual.text == expected.text && actual.quad == expected.quad &&
                        actual.paragraph == expected.paragraph &&
                        actual.sourceLineQuads == expected.sourceLineQuads &&
                        actual.direction == expected.direction,
                    "pin persistence must retain paragraph geometry and rendering metadata");
            auto* content = window->findChild<ScreenshotRecognitionWindow*>();
            auto* layer =
                content == nullptr
                    ? nullptr
                    : content->findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
            // Clipboard handoff is asynchronous and the OS clipboard is a
            // shared resource, so the copied text needs a bounded settle
            // before the visibility and copy assertions can be judged.
            bool copied = false;
            if (layer != nullptr && layer->isVisible() && ocrGlyphItems(layer).size() == 1) {
                for (int attempt = 0; attempt < 40 && !copied; ++attempt) {
                    if (content->copyVisibleContentToClipboard() &&
                        QApplication::clipboard()->text() == expected.text) {
                        copied = true;
                        break;
                    }
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(50);
                }
            }
            require(copied, "merged pinned paragraphs must remain visible and copyable");
            const auto record = window->persistenceSnapshot();
            require(record.textType == snow_shot::storage::PinnedTextType::TranslationResults,
                    "initial and restored screenshot translations select translation results");
            mergedConfig.recognitionResults = {};
            mergedConfig.persistedRecognitionResults = record.recognitionResults;
            mergedConfig.persistedTextType = record.textType;
            mergedConfig.persistedRecognitionVisible = record.recognitionVisible;
            mergedConfig.persistedTranslationVisible = record.translationVisible;
            require(mergedServer.streams.size() == 1, "pinning must not retranslate merged text");
            window->close();
            waitUntil([&]() { return guard.isNull(); }, "close merged translation pin");
        }
    }
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe seed(&api, 2);
    seed.source->lines[1].direction = ScreenshotOcrTextDirection::Vertical;
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "pinned translation requires a test screen");
    ScreenshotPinnedWindow::Config config;
    config.canvasSourceRect = seed.source->selection;
    config.nativeGeometry =
        QRect(ScreenshotGeometryMapper::physicalRectForScreen(*screen).topLeft() + QPoint(40, 40),
              seed.source->selection.size());
    QImage background(seed.source->selection.size(), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(40, 80, 120));
    config.imageSource = ScreenshotImageSource::fromImage(background, config.canvasSourceRect);
    config.screen = screen;
    config.automaticTextRecognition = false;
    config.tableRecognition = &api;
    config.recognitionResults = seed.controller->cachedRecognitionResults();
    auto* pinned = new ScreenshotPinnedWindow;
    QPointer<ScreenshotPinnedWindow> guarded(pinned);
    require(pinned->present(config), "present pinned translation fixture");
    auto* controller = pinned->findChild<ScreenshotRecognitionSessionController*>();
    require(controller != nullptr, "pinned window should own its recognition controller");
    waitUntil([&]() { return controller->hasTextResult(); }, "seed pinned OCR results");
    controller->activate(ScreenshotRecognitionSessionController::Mode::Text);
    controller->beginTextTranslation();
    server.waitForStreams(2);
    auto* content = pinned->findChild<ScreenshotRecognitionWindow*>();
    auto* layer = content == nullptr
                      ? nullptr
                      : content->findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(layer != nullptr && layer->isVisible() && ocrGlyphItems(layer).size() == 2,
            "pinned translation should display both OCR boxes in the embedded text layer");
    const auto items = ocrGlyphItems(layer);
    server.delta(0, QStringLiteral("translated"));
    waitUntil([&]() { return controller->textDraft().startsWith(QStringLiteral("translated")); },
              "pinned translation should consume streamed text");
    require(ocrGlyphItems(layer) == items && controller->recognitionClipboardMimeData()->text() ==
                                                 QStringLiteral("translated\nsource 1"),
            "pinned overlay must update existing text items and copy translated display text");
    {
        const snow_shot::storage::ScreenshotTranslationSettings settings;
        const auto restore = qScopeGuard(
            [&] { static_cast<void>(settings.setCopyTranslationResultsAsImage(false)); });
        require(settings.setCopyTranslationResultsAsImage(true),
                "enable pinned translation image copy");
        const auto mime = controller->recognitionClipboardMimeData();
        require(mime && mime->hasImage() && !mime->hasText() &&
                    qvariant_cast<QImage>(mime->imageData()).size() == background.size(),
                "pinned translation copies its full rendered image at source resolution");
        require(content->copyVisibleContentToClipboard() &&
                    QApplication::clipboard()->mimeData()->hasImage(),
                "embedded pinned copy uses the shared translation policy");
    }
    const auto partialRecord = pinned->persistenceSnapshot();
    const auto partialResults = controller->recognitionResultsSnapshot();
    QByteArray legacyPayload;
    {
        QDataStream stream(&legacyPayload, QIODevice::WriteOnly);
        const auto& source = *partialResults.text->presentation;
        stream << partialResults.key << quint8(1) << quint8(0) << quint8(0)
               << partialResults.text->error << source.selection << qint64(source.lines.size());
        for (const auto& line : source.lines) {
            stream << line.text << line.confidence << line.quad
                   << quint8(line.direction == ScreenshotOcrTextDirection::Vertical ? 1 : 0);
        }
        stream << quint32(0x53535452) << quint8(1)
               << QStringList{QStringLiteral("translated"), QStringLiteral("source 1")};
    }
    require(partialRecord.recognitionVisible && partialRecord.translationVisible &&
                !partialRecord.recognitionResults.isEmpty(),
            "a translating pin must persist the visible partial overlay");
    auto* canvas = pinned->findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "find pinned canvas for wheel zoom");
    const QSize beforeZoom = pinned->currentNativeGeometry().size();
    const QPoint zoomPosition = canvas->rect().center();
    QWheelEvent zoom(QPointF(zoomPosition), QPointF(canvas->mapToGlobal(zoomPosition)), QPoint(),
                     QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &zoom);
    waitUntil([&]() { return pinned->currentNativeGeometry().size() != beforeZoom; },
              "pinned translation should remain zoomable during streaming");
    QContextMenuEvent rotationMenuEvent(QContextMenuEvent::Mouse, QPoint(8, 8),
                                        pinned->mapToGlobal(QPoint(8, 8)));
    QCoreApplication::sendEvent(pinned, &rotationMenuEvent);
    auto* rotationMenu = pinned->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    auto* processAction =
        pinned->findChild<QAction*>(QStringLiteral("screenshotPinnedProcessImageMenu"));
    require(rotationMenu != nullptr && processAction != nullptr && processAction->menu() != nullptr,
            "open the pinned image-processing menu");
    require(QMetaObject::invokeMethod(processAction->menu(), "aboutToShow", Qt::DirectConnection),
            "populate the lazy pinned image-processing menu");
    auto* rotate =
        pinned->findChild<QAction*>(QStringLiteral("screenshotPinnedRotateClockwiseAction"));
    require(rotate != nullptr, "find pinned rotation action");
    rotate->trigger();
    rotationMenu->dismissPopup();
    QCoreApplication::processEvents();
    pinned->resize(pinned->width() + 20, pinned->height() + 10);
    QCoreApplication::processEvents();
    const auto rotatedItems = ocrGlyphItems(layer);
    server.delta(0, QStringLiteral(" after rotation"));
    server.delta(1, QStringLiteral("vertical result"));
    waitUntil([&]() { return controller->textDraft().contains(QStringLiteral("vertical result")); },
              "streaming should continue through pinned rotation and resize");
    require(ocrGlyphItems(layer) == rotatedItems &&
                controller->recognitionClipboardMimeData()->text() ==
                    QStringLiteral("translated after rotation\nvertical result") &&
                seed.source->lines[0].text == QStringLiteral("source 0"),
            "zoomed, rotated and resized pins must retain text mapping and immutable source OCR");
    server.finish(0);
    server.finish(1);
    waitUntil([&]() { return server.disconnected(0) && server.disconnected(1); },
              "finish pinned streams");
    pinned->close();
    waitUntil([&]() { return guarded.isNull(); },
              "closing the pin should release its translation owner");
    config.tableRecognition = nullptr;
    configureTranslation(false);
    auto sourceViewRecord = partialRecord;
    for (const int scenario : {0, 1, 2, 3, 4}) {
        const bool visible = scenario != 2;
        const bool translated = scenario != 3;
        config.restorePersistentState = scenario != 0;
        config.recognitionResults = scenario == 0 ? partialResults : ScreenshotRecognitionResults{};
        config.recognitionVisible = visible;
        config.translationVisible = translated;
        config.persistedRecognitionResults =
            scenario == 3 ? sourceViewRecord.recognitionResults : partialRecord.recognitionResults;
        if (scenario == 4) {
            config.persistedRecognitionResults = legacyPayload;
        }
        config.persistedRecognitionVisible =
            scenario == 3 ? sourceViewRecord.recognitionVisible : visible;
        config.persistedTranslationVisible =
            scenario == 3 ? sourceViewRecord.translationVisible : visible && translated;
        config.persistedTextType =
            scenario == 3 ? sourceViewRecord.textType : partialRecord.textType;
        auto* restored = new ScreenshotPinnedWindow;
        QPointer<ScreenshotPinnedWindow> restoredGuard(restored);
        require(restored->present(config), "recreate a saved translation pin");
        auto* session = restored->findChild<ScreenshotRecognitionSessionController*>();
        waitUntil([&]() { return session->hasTextResult(); }, "restore cached translated text");
        require(session->active() == visible &&
                    restored->persistenceSnapshot().recognitionVisible == visible &&
                    session->originalImageTranslationActive() == (visible && translated) &&
                    restored->persistenceSnapshot().translationVisible == (visible && translated),
                "saved overlay visibility must survive recreation");
        require(session->cachedRecognitionResults().text->presentation->lines[0].text ==
                        QStringLiteral("source 0") &&
                    session->recognitionResultsSnapshot().translatedText->lines[0].text ==
                        QStringLiteral("translated"),
                "restoration must retain separate original OCR and frozen partial translation");
        require(restored->findChild<ScreenshotPinnedEditController*>() == nullptr,
                "initial and restored translations must keep the toolbar hidden");
        if (visible) {
            auto* restoredContent = restored->findChild<ScreenshotRecognitionWindow*>();
            // The OS clipboard handoff is asynchronous; bound the settle
            // before judging the copied payload.
            bool restoredCopied = false;
            const QString restoredExpected = translated ? QStringLiteral("translated\nsource 1")
                                                        : QStringLiteral("source 0\nsource 1");
            if (restoredContent != nullptr && restoredContent->isVisible()) {
                for (int attempt = 0; attempt < 40 && !restoredCopied; ++attempt) {
                    if (restoredContent->copyVisibleContentToClipboard() &&
                        QApplication::clipboard()->text() == restoredExpected) {
                        restoredCopied = true;
                        break;
                    }
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(50);
                }
            }
            require(
                restoredCopied,
                "a restored translated overlay must be visible and copyable without new requests");
            if (scenario == 1) {
                QContextMenuEvent drawingMenuEvent(QContextMenuEvent::Mouse, QPoint(8, 8),
                                                   restored->mapToGlobal(QPoint(8, 8)));
                QCoreApplication::sendEvent(restored, &drawingMenuEvent);
                auto* drawingMenu = restored->findChild<adqt::widgets::AdContextMenu*>(
                    QStringLiteral("screenshotPinnedContextMenu"));
                auto* drawingAction =
                    restored->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
                require(drawingMenu != nullptr && drawingAction != nullptr,
                        "find the toolbar menu action");
                drawingAction->trigger();
                require(drawingAction->isChecked(),
                        "the toolbar menu action must reflect the open toolbar");
                drawingMenu->dismissPopup();
            } else {
                auto* editButton = restored->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotPinnedEditButton"));
                require(editButton != nullptr, "find the toolbar activation control");
                editButton->click();
            }
            auto* editor = restored->findChild<ScreenshotPinnedEditController*>();
            require(editor != nullptr && editor->toolbarWindow() != nullptr &&
                        editor->toolbarWindow()->isVisible() && session->active() &&
                        session->translating() == translated,
                    "opening the toolbar must preserve the active recognition presentation");
            auto* toggle = editor->toolbarWindow()->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotOcrTextTranslateButton"));
            require(toggle != nullptr && toggle->isVisible() && toggle->isEnabled(),
                    "the toolbar must expose the active translation toggle immediately");
            if (translated) {
                toggle->click();
                require(!session->translating() && session->active() &&
                            session->recognitionClipboardMimeData()->text() ==
                                QStringLiteral("source 0\nsource 1") &&
                            !restored->persistenceSnapshot().translationVisible,
                        "canceling translation must display and persist the original OCR view");
                sourceViewRecord = restored->persistenceSnapshot();
            }
            toggle->click();
            require(session->originalImageTranslationActive() &&
                        session->recognitionClipboardMimeData()->text() ==
                            QStringLiteral("translated\nsource 1"),
                    "translation can be re-enabled from its separate cache without a provider");
        }
        require(server.streams.size() == 2, "restoration must not restart translation");
        restored->close();
        waitUntil([&]() { return restoredGuard.isNull(); }, "close restored translation pin");
    }
    for (const int invalidTranslation : {0, 1, 2}) {
        config.recognitionResults = partialResults;
        config.recognitionResults.translatedText.reset();
        if (invalidTranslation == 1) {
            config.recognitionResults.translatedText =
                std::make_shared<ScreenshotOcrPresentation>();
        } else if (invalidTranslation == 2) {
            config.recognitionResults.translatedText =
                std::make_shared<ScreenshotOcrPresentation>();
            config.recognitionResults.translatedText->selection =
                partialResults.translatedText->selection;
            config.recognitionResults.translatedText->lines = partialResults.translatedText->lines;
            config.recognitionResults.translatedText->lines[0].quad.translate(10, 10);
        }
        config.restorePersistentState = false;
        config.persistedRecognitionResults.clear();
        config.recognitionVisible = true;
        config.translationVisible = true;
        auto* fallback = new ScreenshotPinnedWindow;
        QPointer<ScreenshotPinnedWindow> fallbackGuard(fallback);
        require(fallback->present(config), "present a pin without valid translation data");
        auto* session = fallback->findChild<ScreenshotRecognitionSessionController*>();
        waitUntil([&]() { return session->active(); }, "display the valid source OCR fallback");
        require(
            session->translating() &&
                session->cachedRecognitionResults().text->presentation->lines[0].text ==
                    QStringLiteral("source 0") &&
                session->cachedRecognitionResults().text->presentation->lines[1].text ==
                    QStringLiteral("source 1") &&
                fallback->persistenceSnapshot().recognitionVisible &&
                fallback->persistenceSnapshot().textType ==
                    snow_shot::storage::PinnedTextType::TranslationResults,
            "missing or invalid cached translation starts translation while retaining source OCR");
        fallback->close();
        waitUntil([&]() { return fallbackGuard.isNull(); }, "close the translation fallback pin");
    }
}
#endif
