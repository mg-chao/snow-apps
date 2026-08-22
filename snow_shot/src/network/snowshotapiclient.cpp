#include "snow_shot/network/snowshotapiclient.h"

#include "snowimageqtcodec.h"

#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
constexpr int kMaximumSide = 2880;
constexpr int kWebpQuality = 75;
constexpr int kRequestTimeoutMs = 35'000;
constexpr int kTranslationTimeoutMs = 120'000;
constexpr qsizetype kMaximumResponseBytes = 4 * 1024 * 1024;

class SystemNetworkProxyFactory final : public QNetworkProxyFactory {
  public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return systemProxyForQuery(query);
    }
};

void configureProxy(QNetworkAccessManager& manager, bool useSystemProxy) {
    if (useSystemProxy) {
        manager.setProxyFactory(new SystemNetworkProxyFactory);
    } else {
        manager.setProxy(QNetworkProxy::NoProxy);
    }
}

QString normalizedBaseUrl(QString value) {
    value = value.trimmed();
    while (value.endsWith(u'/')) {
        value.chop(1);
    }
    return value;
}

QString problemDetail(const QJsonObject& object) {
    const QString detail = object.value(QStringLiteral("detail")).toString().trimmed();
    if (!detail.isEmpty()) {
        return detail;
    }
    return object.value(QStringLiteral("message")).toString().trimmed();
}

QString translationSystemPrompt(const SnowShotTranslationRequest& request) {
    return QStringLiteral(
               "You are a translation engine. Translate from %1 to %2. If the source language "
               "is auto, detect it. Return only the translated text. Preserve paragraphs and line "
               "breaks. Do not add explanations, headings, notes, markdown wrappers, or "
               "conversational commentary. Preserve URLs, numbers, and symbols unless translation "
               "requires changing them.")
        .arg(request.sourceLanguage, request.targetLanguage);
}
} // namespace

struct SnowShotApiClient::Request {
    enum class Kind { Table, ChatModels, Translation };

    Kind kind = Kind::Table;
    QPointer<QObject> receiver;
    Completion completion;
    ChatModelsCompletion chatModelsCompletion;
    TranslationDelta translationDelta;
    TranslationCompletion translationCompletion;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> timeout;
    QByteArray streamBuffer;
    bool streamDone = false;
    bool streamFailed = false;
};

SnowShotApiClient::SnowShotApiClient(QString baseUrl, QObject* parent)
    : QObject(parent), m_baseUrl(normalizedBaseUrl(std::move(baseUrl))) {}

SnowShotApiClient::~SnowShotApiClient() {
    const auto tokens = m_requests.keys();
    for (const RequestToken token : tokens) {
        cancel(token);
    }
}

const QString& SnowShotApiClient::baseUrl() const {
    return m_baseUrl;
}

bool SnowShotApiClient::usesSystemProxy() const {
    return m_useSystemProxy;
}

void SnowShotApiClient::setUseSystemProxy(bool enabled) {
    if (m_useSystemProxy == enabled) {
        return;
    }
    m_useSystemProxy = enabled;
    if (auto* manager = findChild<QNetworkAccessManager*>(); manager != nullptr) {
        configureProxy(*manager, m_useSystemProxy);
    }
}

QNetworkAccessManager* SnowShotApiClient::networkAccessManager() {
    auto* manager = findChild<QNetworkAccessManager*>();
    if (manager == nullptr) {
        manager = new QNetworkAccessManager(this);
        configureProxy(*manager, m_useSystemProxy);
    }
    return manager;
}

QImage SnowShotApiClient::prepareImage(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    const int longestSide = std::max(image.width(), image.height());
    if (longestSide <= kMaximumSide) {
        return image;
    }
    return image.scaled(kMaximumSide, kMaximumSide, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
}

QByteArray SnowShotApiClient::encodeWebp(const QImage& image) {
    return snow_shot::image_codec::encodeWebp(image, kWebpQuality);
}

QString SnowShotApiClient::formatFailure(int httpStatus, const QString& failureCode,
                                         const QString& description) {
    const QString conciseDescription = description.simplified();
    const QString code = httpStatus > 0 && httpStatus != 200
                             ? QString::number(httpStatus)
                             : failureCode.trimmed();
    if (code.isEmpty()) {
        return conciseDescription;
    }
    if (conciseDescription.isEmpty()) {
        return code;
    }
    return QStringLiteral("%1: %2").arg(code, conciseDescription);
}

SnowShotApiClient::RequestToken SnowShotApiClient::extractTable(const QImage& source,
                                                               QObject* receiver,
                                                               Completion completion) {
    if (receiver == nullptr || !completion || m_baseUrl.isEmpty()) {
        return 0;
    }

    const QByteArray webp = encodeWebp(prepareImage(source));
    if (webp.isEmpty()) {
        return 0;
    }

    auto* manager = networkAccessManager();

    const RequestToken token = ++m_nextToken;
    auto* requestState = new Request;
    requestState->receiver = receiver;
    requestState->completion = std::move(completion);
    m_requests.insert(token, requestState);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/api/v1/table/extract")));
    request.setRawHeader("X-Request-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    request.setTransferTimeout(kRequestTimeoutMs);

    auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart imagePart;
    imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QVariant(QStringLiteral("form-data; name=\"image\"; filename=\"table.webp\"")));
    imagePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/webp"));
    imagePart.setBody(webp);
    multipart->append(imagePart);

    QNetworkReply* reply = manager->post(request, multipart);
    multipart->setParent(reply);
    requestState->reply = reply;

    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    timeout->setInterval(kRequestTimeoutMs);
    requestState->timeout = timeout;
    connect(timeout, &QTimer::timeout, reply, [reply]() {
        if (reply != nullptr && reply->isRunning()) {
            reply->abort();
        }
    });
    timeout->start();

    connect(reply, &QNetworkReply::finished, this, [this, token, reply]() {
        if (!m_requests.contains(token)) {
            reply->deleteLater();
            return;
        }

        SnowShotTableResult result;
        result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->read(kMaximumResponseBytes + 1);
        if (body.size() > kMaximumResponseBytes) {
            result.error = tr("Table recognition response is too large");
        } else {
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
            const QJsonObject root = document.isObject() ? document.object() : QJsonObject{};
            result.code = root.value(QStringLiteral("code")).toVariant().toString();

            if (reply->error() != QNetworkReply::NoError) {
                if (reply->error() == QNetworkReply::OperationCanceledError) {
                    result.error = tr("Table recognition request timed out");
                } else {
                    QString description = problemDetail(root);
                    if (description.isEmpty() && result.httpStatus > 0) {
                        description = reply
                                          ->attribute(QNetworkRequest::HttpReasonPhraseAttribute)
                                          .toString();
                    }
                    if (description.isEmpty()) {
                        description = reply->errorString();
                    }
                    result.error = formatFailure(result.httpStatus, result.code, description);
                }
            } else if (!document.isObject()) {
                result.error = tr("Invalid table recognition response");
            } else {
                if (result.httpStatus == 200 && root.value(QStringLiteral("data")).isObject()) {
                    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
                    result.html = data.value(QStringLiteral("html")).toString();
                    if (result.html.trimmed().isEmpty()) {
                        result.error = tr("Table recognition returned no table");
                    }
                } else {
                    result.error = problemDetail(root);
                    if (result.error.isEmpty()) {
                        result.error = tr("Table recognition failed");
                    }
                    result.error = formatFailure(result.httpStatus, result.code, result.error);
                }
            }
        }
        finish(token, std::move(result));
    });

    return token;
}

const QVector<SnowShotChatModel>& SnowShotApiClient::cachedChatModels() const {
    return m_cachedChatModels;
}

SnowShotApiClient::RequestToken SnowShotApiClient::fetchChatModels(
    const QString& locale, QObject* receiver, ChatModelsCompletion completion) {
    if (receiver == nullptr || !completion || m_baseUrl.isEmpty()) {
        return 0;
    }
    auto* manager = networkAccessManager();
    const RequestToken token = ++m_nextToken;
    auto* state = new Request;
    state->kind = Request::Kind::ChatModels;
    state->receiver = receiver;
    state->chatModelsCompletion = std::move(completion);
    m_requests.insert(token, state);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/api/v1/chat/models")));
    request.setRawHeader("X-Request-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    if (!locale.trimmed().isEmpty()) {
        request.setRawHeader("Accept-Language", locale.toUtf8());
    }
    request.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply* reply = manager->get(request);
    state->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, token, reply]() {
        if (!m_requests.contains(token)) {
            reply->deleteLater();
            return;
        }
        SnowShotChatModelsResult result;
        result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->read(kMaximumResponseBytes + 1);
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        const QJsonObject root = document.isObject() ? document.object() : QJsonObject{};
        result.code = root.value(QStringLiteral("code")).toVariant().toString();
        if (body.size() > kMaximumResponseBytes) {
            result.error = tr("Translation service response is too large");
        } else if (reply->error() != QNetworkReply::NoError) {
            result.error = formatFailure(result.httpStatus, result.code,
                                         problemDetail(root).isEmpty() ? reply->errorString()
                                                                       : problemDetail(root));
        } else if (!document.isObject() || !root.value(QStringLiteral("data")).isArray()) {
            result.error = tr("Invalid translation service response");
        } else {
            for (const QJsonValue& value : root.value(QStringLiteral("data")).toArray()) {
                const QJsonObject model = value.toObject();
                SnowShotChatModel parsed{model.value(QStringLiteral("model")).toString().trimmed(),
                                         model.value(QStringLiteral("name")).toString().trimmed(),
                                         model.value(QStringLiteral("thinking")).toBool(),
                                         model.value(QStringLiteral("support_vision")).toBool()};
                if (!parsed.id.isEmpty() && !parsed.name.isEmpty()) {
                    result.models.push_back(std::move(parsed));
                }
            }
            if (result.models.isEmpty()) {
                result.error = tr("No translation services are available");
            } else {
                m_cachedChatModels = result.models;
            }
        }
        finishChatModels(token, std::move(result));
        reply->deleteLater();
    });
    return token;
}

SnowShotApiClient::RequestToken SnowShotApiClient::streamTranslation(
    const SnowShotTranslationRequest& input, QObject* receiver, TranslationDelta delta,
    TranslationCompletion completion) {
    if (receiver == nullptr || !delta || !completion || m_baseUrl.isEmpty() ||
        input.model.trimmed().isEmpty() || input.text.isEmpty()) {
        return 0;
    }
    auto* manager = networkAccessManager();
    const RequestToken token = ++m_nextToken;
    auto* state = new Request;
    state->kind = Request::Kind::Translation;
    state->receiver = receiver;
    state->translationDelta = std::move(delta);
    state->translationCompletion = std::move(completion);
    m_requests.insert(token, state);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/api/v1/chat/completions")));
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("X-Request-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    request.setTransferTimeout(kTranslationTimeoutMs);
    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                    {QStringLiteral("content"), translationSystemPrompt(input)}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), input.text}},
    };
    const QJsonObject body{{QStringLiteral("model"), input.model},
                           {QStringLiteral("messages"), messages},
                           {QStringLiteral("enable_thinking"), false},
                           {QStringLiteral("temperature"), 0},
                           {QStringLiteral("max_tokens"), 4096}};
    QNetworkReply* reply = manager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    state->reply = reply;

    const auto parseAvailable = [this, token]() {
        Request* current = m_requests.value(token, nullptr);
        if (current == nullptr || current->reply == nullptr) {
            return;
        }
        current->streamBuffer += current->reply->readAll();
        while (true) {
            qsizetype separator = current->streamBuffer.indexOf("\n\n");
            qsizetype separatorSize = 2;
            const qsizetype crlfSeparator = current->streamBuffer.indexOf("\r\n\r\n");
            if (crlfSeparator >= 0 && (separator < 0 || crlfSeparator < separator)) {
                separator = crlfSeparator;
                separatorSize = 4;
            }
            if (separator < 0) {
                break;
            }
            QByteArray event = current->streamBuffer.left(separator);
            current->streamBuffer.remove(0, separator + separatorSize);
            event.replace("\r\n", "\n");
            QByteArray eventName;
            QByteArray data;
            for (const QByteArray& line : event.split('\n')) {
                if (line.startsWith("event:")) {
                    eventName = line.mid(6).trimmed();
                } else if (line.startsWith("data:")) {
                    if (!data.isEmpty()) {
                        data += '\n';
                    }
                    data += line.mid(5).trimmed();
                }
            }
            if (data == "[DONE]") {
                current->streamDone = true;
                continue;
            }
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(data, &error);
            const QJsonObject object = document.isObject() ? document.object() : QJsonObject{};
            if (eventName == "error") {
                current->streamFailed = true;
                SnowShotTranslationResult result;
                result.error = problemDetail(object);
                if (result.error.isEmpty()) {
                    result.error = tr("Translation failed");
                }
                result.code = object.value(QStringLiteral("code")).toVariant().toString();
                finishTranslation(token, std::move(result));
                return;
            }
            if (data.isEmpty()) {
                continue;
            }
            if (!document.isObject()) {
                current->streamFailed = true;
                SnowShotTranslationResult result;
                result.error = tr("Invalid translation stream response");
                finishTranslation(token, std::move(result));
                return;
            }
            QString content;
            for (const QJsonValue& choice : object.value(QStringLiteral("choices")).toArray()) {
                content += choice.toObject().value(QStringLiteral("delta")).toObject()
                               .value(QStringLiteral("content"))
                               .toString();
            }
            if (!content.isEmpty() && current->receiver != nullptr && current->translationDelta) {
                current->translationDelta(content);
            }
        }
    };
    connect(reply, &QIODevice::readyRead, this, parseAvailable);
    connect(reply, &QNetworkReply::finished, this, [this, token, reply, parseAvailable]() {
        if (!m_requests.contains(token)) {
            reply->deleteLater();
            return;
        }
        parseAvailable();
        Request* current = m_requests.value(token, nullptr);
        if (current == nullptr) {
            reply->deleteLater();
            return;
        }
        SnowShotTranslationResult result;
        result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            const QJsonObject problem = QJsonDocument::fromJson(current->streamBuffer).object();
            result.error = formatFailure(result.httpStatus, {},
                                         problemDetail(problem).isEmpty() ? reply->errorString()
                                                                         : problemDetail(problem));
        } else if (current->streamFailed) {
            result.error = tr("Invalid translation stream response");
        } else if (!current->streamDone) {
            result.error = tr("Translation stream ended unexpectedly");
        }
        finishTranslation(token, std::move(result));
        reply->deleteLater();
    });
    return token;
}

void SnowShotApiClient::cancel(RequestToken token) {
    auto it = m_requests.find(token);
    if (it == m_requests.end()) {
        return;
    }
    Request* request = it.value();
    m_requests.erase(it);
    if (request->timeout != nullptr) {
        request->timeout->stop();
    }
    if (request->reply != nullptr && request->reply->isRunning()) {
        request->reply->abort();
    }
    delete request;
}

void SnowShotApiClient::finish(RequestToken token, SnowShotTableResult result) {
    auto it = m_requests.find(token);
    if (it == m_requests.end()) {
        return;
    }
    Request* request = it.value();
    m_requests.erase(it);
    if (request->timeout != nullptr) {
        request->timeout->stop();
    }
    const QPointer<QObject> receiver = request->receiver;
    Completion completion = std::move(request->completion);
    delete request;
    if (receiver != nullptr && completion) {
        completion(std::move(result));
    }
}

void SnowShotApiClient::finishChatModels(RequestToken token, SnowShotChatModelsResult result) {
    auto it = m_requests.find(token);
    if (it == m_requests.end()) {
        return;
    }
    Request* request = it.value();
    m_requests.erase(it);
    const QPointer<QObject> receiver = request->receiver;
    ChatModelsCompletion completion = std::move(request->chatModelsCompletion);
    delete request;
    if (receiver != nullptr && completion) {
        completion(std::move(result));
    }
}

void SnowShotApiClient::finishTranslation(RequestToken token, SnowShotTranslationResult result) {
    auto it = m_requests.find(token);
    if (it == m_requests.end()) {
        return;
    }
    Request* request = it.value();
    m_requests.erase(it);
    const QPointer<QObject> receiver = request->receiver;
    TranslationCompletion completion = std::move(request->translationCompletion);
    delete request;
    if (receiver != nullptr && completion) {
        completion(std::move(result));
    }
}
