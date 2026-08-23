#include "snow_shot/network/snowshotapiclient.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QByteArray waitForHttpRequest(QTcpServer& server, const QByteArray& response) {
    QByteArray request;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&]() {
        QTcpSocket* socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, &loop, [&, socket]() {
            request += socket->readAll();
            const qsizetype headerEnd = request.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            qsizetype contentLength = 0;
            for (const QByteArray& line : request.left(headerEnd).split('\n')) {
                if (line.toLower().startsWith("content-length:")) {
                    contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                }
            }
            if (request.size() < headerEnd + 4 + contentLength) {
                return;
            }
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            loop.quit();
        });
    });
    timeout.start();
    loop.exec();
    require(timeout.isActive(), "local API test server timed out waiting for a request");
    return request;
}

void apiClientUsesModelCatalogAndStreamingChatContracts() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "local API test server should listen");
    const QString baseUrl =
        QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    SnowShotApiClient client(baseUrl);

    SnowShotChatModelsResult modelsResult;
    bool modelsFinished = false;
    QEventLoop modelsCompletionLoop;
    const auto modelsToken = client.fetchChatModels(
        QStringLiteral("zh-CN"), &client, [&](SnowShotChatModelsResult result) {
            modelsResult = std::move(result);
            modelsFinished = true;
            modelsCompletionLoop.quit();
        });
    require(modelsToken != 0, "model catalog request should be prepared");
    const QByteArray modelsBody = QByteArrayLiteral(
        R"({"code":0,"message":"ok","data":[{"model":"model-a","name":"Model A","thinking":true,"support_vision":false}]})");
    const QByteArray modelsResponse =
        QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ") +
        QByteArray::number(modelsBody.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") +
        modelsBody;
    const QByteArray modelsRequest = waitForHttpRequest(server, modelsResponse);
    if (!modelsFinished) {
        QTimer::singleShot(5000, &modelsCompletionLoop, &QEventLoop::quit);
        modelsCompletionLoop.exec();
    }
    require(modelsFinished && modelsResult.succeeded() && modelsResult.models.size() == 1 &&
                modelsResult.models.first().id == QStringLiteral("model-a") &&
                modelsResult.models.first().name == QStringLiteral("Model A") &&
                modelsResult.models.first().thinking &&
                !modelsResult.models.first().supportsVision,
            "model catalog envelope should preserve the public API descriptor fields");
    require(modelsRequest.startsWith("GET /api/v1/chat/models HTTP/1.1") &&
                modelsRequest.toLower().contains("accept-language: zh-cn"),
            "model catalog request should use the documented endpoint and locale header");
    auto* manager = client.findChild<QNetworkAccessManager*>();
    require(manager != nullptr && manager->proxy().type() == QNetworkProxy::NoProxy &&
                manager->proxyFactory() == nullptr && !client.usesSystemProxy(),
            "network requests must bypass proxies by default");
    client.setUseSystemProxy(true);
    require(client.usesSystemProxy() && manager->proxyFactory() != nullptr,
            "system proxy mode must install system proxy resolution on the request manager");
    client.setUseSystemProxy(false);
    require(!client.usesSystemProxy() && manager->proxy().type() == QNetworkProxy::NoProxy &&
                manager->proxyFactory() == nullptr,
            "disabling proxy mode must restore explicit no-proxy requests");

    QString streamedText;
    SnowShotTranslationResult translationResult;
    bool translationFinished = false;
    QEventLoop translationCompletionLoop;
    const auto translationToken = client.streamTranslation(
        SnowShotTranslationRequest{QStringLiteral("model-a"), QStringLiteral("English"),
                                   QStringLiteral("Simplified Chinese"),
                                   QStringLiteral("Hello\nworld")},
        &client, [&](const QString& delta) { streamedText += delta; },
        [&](SnowShotTranslationResult result) {
            translationResult = std::move(result);
            translationFinished = true;
            translationCompletionLoop.quit();
        });
    require(translationToken != 0, "streaming translation request should be prepared");
    const QByteArray streamBody = QByteArrayLiteral(
        "data: {\"choices\":[{\"delta\":{\"content\":\"Ni hao\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" shijie\"}}]}\r\n\r\n"
        "data: [DONE]\n\n");
    const QByteArray streamResponse = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: ") +
        QByteArray::number(streamBody.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") +
        streamBody;
    const QByteArray translationRequest = waitForHttpRequest(server, streamResponse);
    if (!translationFinished) {
        QTimer::singleShot(5000, &translationCompletionLoop, &QEventLoop::quit);
        translationCompletionLoop.exec();
    }
    require(translationFinished && translationResult.succeeded() &&
                streamedText == QStringLiteral("Ni hao shijie"),
            "SSE chat deltas should be delivered in order and complete only at the done marker");
    require(translationRequest.startsWith("POST /api/v1/chat/completions HTTP/1.1"),
            "translation should use the documented streaming chat endpoint");
    const qsizetype bodyOffset = translationRequest.indexOf("\r\n\r\n") + 4;
    const QJsonObject requestBody =
        QJsonDocument::fromJson(translationRequest.mid(bodyOffset)).object();
    require(requestBody.value(QStringLiteral("model")).toString() == QStringLiteral("model-a") &&
                !requestBody.value(QStringLiteral("enable_thinking")).toBool(true) &&
                requestBody.value(QStringLiteral("temperature")).toDouble(-1.0) == 0.0 &&
                requestBody.value(QStringLiteral("max_tokens")).toInt() == 4096,
            "translation chat request should use deterministic non-thinking model settings");
    const QJsonArray messages = requestBody.value(QStringLiteral("messages")).toArray();
    require(messages.size() == 2 &&
                messages.at(0).toObject().value(QStringLiteral("role")).toString() ==
                    QStringLiteral("system") &&
                messages.at(0).toObject().value(QStringLiteral("content")).toString().contains(
                    QStringLiteral("Return only the translated text")) &&
                messages.at(1).toObject().value(QStringLiteral("content")).toString() ==
                    QStringLiteral("Hello\nworld"),
            "translation request should carry the translation-only system prompt and original text");

    SnowShotTranslationResult malformedResult;
    bool malformedFinished = false;
    QEventLoop malformedCompletionLoop;
    const auto malformedToken = client.streamTranslation(
        SnowShotTranslationRequest{QStringLiteral("model-a"), QStringLiteral("English"),
                                   QStringLiteral("German"), QStringLiteral("Hello")},
        &client, [](const QString&) {}, [&](SnowShotTranslationResult result) {
            malformedResult = std::move(result);
            malformedFinished = true;
            malformedCompletionLoop.quit();
        });
    require(malformedToken != 0, "malformed-stream test request should be prepared");
    const QByteArray malformedBody = QByteArrayLiteral("data: not-json\n\ndata: [DONE]\n\n");
    const QByteArray malformedResponse = QByteArrayLiteral(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: ") +
        QByteArray::number(malformedBody.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") +
        malformedBody;
    static_cast<void>(waitForHttpRequest(server, malformedResponse));
    if (!malformedFinished) {
        QTimer::singleShot(5000, &malformedCompletionLoop, &QEventLoop::quit);
        malformedCompletionLoop.exec();
    }
    require(malformedFinished && !malformedResult.succeeded() &&
                !malformedResult.error.isEmpty(),
            "a malformed nonempty SSE frame should fail even when followed by a done marker");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);

    QImage wide(4000, 2000, QImage::Format_RGBA8888);
    wide.fill(Qt::white);
    const QImage preparedWide = SnowShotApiClient::prepareImage(wide);
    require(preparedWide.size() == QSize(2880, 1440),
            "wide images should scale proportionally to a 2880px longest side");

    QImage small(1280, 720, QImage::Format_RGBA8888);
    small.fill(Qt::black);
    require(SnowShotApiClient::prepareImage(small).size() == small.size(),
            "small images should not be upscaled");

    const QByteArray webp = SnowShotApiClient::encodeWebp(small);
    require(webp.size() > 12 && webp.left(4) == QByteArrayLiteral("RIFF") &&
                webp.mid(8, 4) == QByteArrayLiteral("WEBP"),
            "table requests should be encoded as WebP");

    require(SnowShotApiClient::formatFailure(503, QStringLiteral("SERVICE_BUSY"),
                                             QStringLiteral("  Service unavailable  ")) ==
                QStringLiteral("503: Service unavailable"),
            "HTTP failures should show only the HTTP status and concise description");
    require(SnowShotApiClient::formatFailure(200, QStringLiteral("TABLE_NOT_FOUND"),
                                             QStringLiteral("No table was detected")) ==
                QStringLiteral("TABLE_NOT_FOUND: No table was detected"),
            "API failures should show the failure code and description");
    require(SnowShotApiClient::formatFailure(0, {}, QStringLiteral("  Connection\nfailed ")) ==
                QStringLiteral("Connection failed"),
            "transport failures without a code should remain concise");
    apiClientUsesModelCatalogAndStreamingChatContracts();
    return 0;
}
