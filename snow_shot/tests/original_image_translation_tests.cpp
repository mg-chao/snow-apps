#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/modal.h"
#include "widgets/switch.h"

#include <QCoreApplication>
#include <QClipboard>
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
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/context_menu.h"
#include "widgets/button.h"
#include <QAction>
#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QScreen>
#include <QWheelEvent>
#endif

namespace {
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
        waitUntil([&]() { return streams.size() >= count; }, "expected translation requests");
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
    void startStream(int index) {
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
    int textUpdates = 0;
    int backgrounds = 0;
    bool streaming = false;
    bool overlayState = false;

    ~SessionProbe() {
        controller.reset();
    }

    SessionProbe(SnowShotApiClient* api, int boxes, QString key = QStringLiteral("image"),
                 bool formatted = false, ScreenshotOcrRecognitionPort* ocr = nullptr,
                 QWidget* settingsOwner = nullptr) {
        ScreenshotRecognitionSessionActions actions;
        actions.translationSettingsOwner = [settingsOwner]() { return settingsOwner; };
        actions.applyOcrPresentation = [this](auto presentation) {
            displayed = std::move(presentation);
        };
        actions.applyOcrBackground = [this](auto) { ++backgrounds; };
        if (ocr != nullptr) {
            actions.applyOcrBackgroundImage = [](auto, QImage, QRectF) {};
        }
        actions.updateOcrText = [this](int index, const QString& text) {
            ++textUpdates;
            require(displayed != source, "stream updates must not modify the source presentation");
            displayed->setLineText(index, text);
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
        source->selection = QRect(0, 0, 160, 240);
        for (int index = 0; index < boxes; ++index) {
            const qreal top = index * 25.0;
            source->lines.push_back({QStringLiteral("source %1").arg(index), 0.95,
                                     QPolygonF{QPointF(0, top), QPointF(150, top),
                                               QPointF(150, top + 20), QPointF(0, top + 20)}});
        }
        source->prepareForRendering();
        ScreenshotRecognitionTarget target;
        target.key = key;
        target.image = QImage(160, 240, QImage::Format_ARGB32_Premultiplied);
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

void queueStreamsIndividualBoxesAndKeepsOriginals() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 6);
    session.controller->beginTextTranslation();
    require(session.streaming && session.overlayState && !session.controller->editing(),
            "overlay mode should be busy during model discovery without entering the editor");
    server.waitForStreams(4);
    require(server.modelRequests == 1, "resolve translation models once per image run");
    for (int index = 0; index < 4; ++index) {
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
            "streaming and queued boxes should retain independent display content");
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
    server.delta(2, QStringLiteral(" box"));
    server.finish(2);
    server.waitForStreams(5);
    require(snapshot.translatedText->lines[2].text == QStringLiteral("third"),
            "later translation deltas must not modify a previously captured pin snapshot");
    require(session.displayed->selectedText() == QStringLiteral("irs") &&
                session.backgrounds == backgrounds,
            "stream updates should preserve unrelated selection and the OCR background");
    server.finish(0);
    server.waitForStreams(6);
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
    SessionProbe closing(&api, 6);
    closing.controller->beginTextTranslation();
    server.waitForStreams(8);
    closing.controller.reset();
    for (int index = 4; index < 8; ++index) {
        waitUntil([&]() { return server.disconnected(index); }, "owner destruction closes streams");
    }
    require(server.streams.size() == 8, "owner destruction must discard queued boxes");
}

void partialFailuresRetryOnlyFailedBoxes() {
    configureTranslation();
    TranslationServer server;
    SnowShotApiClient api(server.url());
    SessionProbe session(&api, 5);
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    server.delta(0, QStringLiteral("partial"));
    server.fail(0);
    server.waitForStreams(5);
    waitUntil([&]() { return server.disconnected(0); },
              "terminal SSE error must close its HTTP reply");
    server.fail(1);
    server.finish(2);
    server.delta(3, QStringLiteral("complete"));
    server.finish(3);
    server.delta(4, QStringLiteral("last"));
    server.finish(4);
    waitUntil([&]() { return !session.streaming; }, "partial failure run should settle");
    require(session.errors.size() == 1, "partial failure should report exactly one error summary");
    require(session.displayed->lines[0].text == QStringLiteral("partial"),
            "the first failed box should keep its partial output");
    require(session.displayed->lines[1].text == QStringLiteral("source 1"),
            "failure before a delta should keep the second source box");
    require(session.displayed->lines[2].text == QStringLiteral("source 2"),
            "empty completion should keep the third source box");
    session.controller->endTextEditing();
    session.controller->beginTextTranslation();
    server.waitForStreams(8);
    require(session.displayed->lines[0].text == QStringLiteral("partial") &&
                session.displayed->lines[3].text == QStringLiteral("complete"),
            "retry should retain previous partial and completed output until new deltas");
    for (int index = 5; index < 8; ++index) {
        require(server.streams[index].text == QStringLiteral("source %1").arg(index - 5),
                "retry only failed boxes using source text");
        server.delta(index, QStringLiteral("retry %1").arg(index - 5));
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
    SessionProbe session(&api, 8);
    session.controller->beginTextTranslation();
    server.waitForStreams(4);
    server.delta(0, QStringLiteral("completed before limit"));
    server.finish(0);
    server.waitForStreams(5);
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
    for (int index = 2; index < 5; ++index) {
        server.delta(index, QStringLiteral("in flight %1").arg(index));
        server.finish(index);
    }
    waitUntil([&] { return !session.streaming || server.streams.size() > 5; },
              "rate-limited batch should stop after its in-flight work finishes");
    require(server.streams.size() == 5, "HTTP 429 must stop dispatch of queued translation boxes");
    require(!session.streaming && session.errors.size() == 1,
            "rate-limited batch should leave busy state and report one failure");
    require(session.displayed->lines[0].text == QStringLiteral("completed before limit") &&
                session.displayed->lines[4].text == QStringLiteral("in flight 4") &&
                session.displayed->lines[1].text == QStringLiteral("source 1") &&
                session.displayed->lines[7].text == QStringLiteral("source 7"),
            "rate limiting must retain translations and the original text of unsent boxes");
    session.controller->endTextEditing();
    session.controller->beginTextTranslation();
    server.waitForStreams(9);
    QStringList retried;
    for (int index = 5; index < 9; ++index) {
        retried.append(server.streams[index].text);
        server.delta(index, QStringLiteral("retried %1").arg(server.streams[index].text));
        server.finish(index);
    }
    require(retried == QStringList{QStringLiteral("source 1"), QStringLiteral("source 5"),
                                   QStringLiteral("source 6"), QStringLiteral("source 7")},
            "explicit retry must send only the rejected and previously unsent boxes");
    waitUntil([&] { return !session.streaming; }, "explicit rate-limit retry should finish");
    require(session.errors.size() == 1 &&
                session.displayed->lines[0].text == QStringLiteral("completed before limit") &&
                session.displayed->lines[7].text == QStringLiteral("retried source 7"),
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
    for (const bool checked : {false, true}) {
        toggle->setChecked(checked);
        QCoreApplication::processEvents();
        require(toggle->sizeHint().width() >= 56 && toggle->sizeHint().height() >= 28,
                "translation switch must request at least a 56 by 28 logical-pixel indicator");
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
    SessionProbe session(&api, 5);
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
                    QStringLiteral("source 0\nsource 1\nsource 2\nsource 3\nsource 4"),
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
            "invalidated queue must never dispatch its remaining box");
}

void languageChangesRestartAndProviderDestructionCancels() {
    configureTranslation();
    TranslationServer server;
    auto api = std::make_unique<SnowShotApiClient>(server.url());
    SessionProbe session(api.get(), 5);
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
            "provider loss should report one failure and discard queued work");
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
    SessionProbe session(&api, 5, QStringLiteral("snapshot"), false, ocr.get());
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
    translationSettingsSwitchKeepsReadableSize();
    confirmingTranslationDisplayModeRestartsTranslation();
    smartLayoutStreamsParagraphsAndSwitchesModes();
    queueStreamsIndividualBoxesAndKeepsOriginals();
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
            require(layer != nullptr && layer->isVisible() && layer->scene()->items().size() == 1 &&
                        content->copyVisibleContentToClipboard() &&
                        QApplication::clipboard()->text() == expected.text,
                    "merged pinned paragraphs must remain visible and copyable");
            const auto record = window->persistenceSnapshot();
            mergedConfig.recognitionResults = {};
            mergedConfig.persistedRecognitionResults = record.recognitionResults;
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
    require(layer != nullptr && layer->isVisible() && layer->scene()->items().size() == 2,
            "pinned translation should display both OCR boxes in the embedded text layer");
    const auto items = layer->scene()->items();
    server.delta(0, QStringLiteral("translated"));
    waitUntil([&]() { return controller->textDraft().startsWith(QStringLiteral("translated")); },
              "pinned translation should consume streamed text");
    require(layer->scene()->items() == items &&
                controller->recognitionClipboardMimeData()->text() ==
                    QStringLiteral("translated\nsource 1"),
            "pinned overlay must update existing text items and copy translated display text");
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
    auto* processMenu = pinned->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedProcessImageMenu"));
    require(processMenu != nullptr && !processMenu->actions().isEmpty(),
            "find pinned rotation action");
    processMenu->actions().first()->trigger();
    QCoreApplication::processEvents();
    pinned->resize(pinned->width() + 20, pinned->height() + 10);
    QCoreApplication::processEvents();
    const auto rotatedItems = layer->scene()->items();
    server.delta(0, QStringLiteral(" after rotation"));
    server.delta(1, QStringLiteral("vertical result"));
    waitUntil([&]() { return controller->textDraft().contains(QStringLiteral("vertical result")); },
              "streaming should continue through pinned rotation and resize");
    require(layer->scene()->items() == rotatedItems &&
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
            require(
                restoredContent != nullptr && restoredContent->isVisible() &&
                    restoredContent->copyVisibleContentToClipboard() &&
                    QApplication::clipboard()->text() ==
                        (translated ? QStringLiteral("translated\nsource 1")
                                    : QStringLiteral("source 0\nsource 1")),
                "a restored translated overlay must be visible and copyable without new requests");
            if (scenario == 1) {
                auto* drawingAction =
                    restored->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction"));
                require(drawingAction != nullptr, "find the toolbar menu action");
                drawingAction->trigger();
                require(drawingAction->isChecked(),
                        "the toolbar menu action must reflect the open toolbar");
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
        require(!session->translating() &&
                    session->recognitionClipboardMimeData()->text() ==
                        QStringLiteral("source 0\nsource 1") &&
                    fallback->persistenceSnapshot().recognitionVisible &&
                    !fallback->persistenceSnapshot().translationVisible,
                "missing or invalid translation must fall back to OCR and clear translation "
                "visibility");
        fallback->close();
        waitUntil([&]() { return fallbackGuard.isNull(); }, "close the translation fallback pin");
    }
}
#endif
