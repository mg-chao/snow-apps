#include "translation_test_support.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationarchive.h"
#include "../src/network/texttranslationprotocol.h"
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <memory>

using namespace translation_tests;
using namespace snow_shot;
class SnowShotApiClientTestAccess {
  public:
    static qsizetype queued(const SnowShotApiClient& client) {
        return client.m_translationQueue.size();
    }
    static qsizetype requests(const SnowShotApiClient& client) {
        return client.m_requests.size();
    }
    static void timeout(SnowShotApiClient& client) {
        const auto timers = client.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
        require(!timers.isEmpty(), "active timeout exists");
        for (auto* timer : timers)
            timer->setInterval(1);
    }
};
namespace {
const QString key = QStringLiteral("api_configuration/text_translation");
TextTranslationConfiguration config(const QString& endpoint,
                                    const QString& provider = QStringLiteral("deepl")) {
    return {QUuid::createUuid().toString(QUuid::WithoutBraces),
            QStringLiteral("Translation"),
            provider,
            endpoint,
            QStringLiteral("secret"),
            QStringLiteral("app"),
            4};
}
class HttpServer : public QObject {
  public:
    struct Request {
        QPointer<QTcpSocket> socket;
        QByteArray headers;
        QByteArray body;
    };
    QTcpServer server;
    QList<Request> requests;
    HttpServer() {
        require(server.listen(QHostAddress::LocalHost), "listen");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool())
                        return;
                    const QByteArray bytes =
                        socket->property("bytes").toByteArray() + socket->readAll();
                    socket->setProperty("bytes", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    qsizetype length = 0;
                    for (const auto& line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    if (bytes.size() < end + 4 + length)
                        return;
                    socket->setProperty("handled", true);
                    requests.append({socket, bytes.left(end), bytes.mid(end + 4, length)});
                });
            }
        });
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1/custom/translate?route=alpha%2Bbeta")
            .arg(server.serverPort());
    }
    void respond(qsizetype index,
                 const QByteArray& bytes =
                     QByteArrayLiteral("{\"translations\":[{\"text\":\"translated\"}]}"),
                 int status = 200) {
        auto socket = requests[index].socket;
        require(socket && socket->state() == QAbstractSocket::ConnectedState,
                "response socket connected");
        socket->write(
            "HTTP/1.1 " + QByteArray::number(status) +
            " Result\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
            QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
};
void persistence() {
    QTemporaryDir directory;
    auto value = config(QStringLiteral("http://localhost/custom/?a=1%2B2"));
    const auto path = directory.filePath(QStringLiteral("config.json"));
    {
        storage::ConfigurationStore store(path, true, true, 8000);
        require(store.value(key).toArray().isEmpty(), "empty default");
        require(store.setValue(key, textTranslationConfigurationsToJson({value})), "save config");
        require(store.flushNow().success, "flush");
        auto duplicate = value;
        duplicate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        duplicate.name = value.name.toUpper();
        require(!store.setValue(key, textTranslationConfigurationsToJson({value, duplicate})),
                "reject duplicate names");
        duplicate.name = QStringLiteral("Second");
        require(store.setValue(key, textTranslationConfigurationsToJson({value, duplicate})),
                "same provider supported twice");
        for (int limit : {0, 17}) {
            duplicate.concurrency = limit;
            require(!store.setValue(key, textTranslationConfigurationsToJson({duplicate})),
                    "bounds rejected");
        }
    }
    storage::ConfigurationStore reopened(path, true, true, 8000);
    require(textTranslationConfigurationsFromJson(reopened.value(key)).first() == value,
            "roundtrip retains endpoint and identity");
    for (const auto& url :
         {QStringLiteral("ftp://host/path"), QStringLiteral("https://user:pass@host/path"),
          QStringLiteral("https://host/path#fragment")})
        require(!validTranslationEndpoint(url), "invalid endpoint rejected");
    auto json = textTranslationConfigurationsToJson({value}).first().toObject();
    json.remove(QStringLiteral("concurrency"));
    require(textTranslationConfigurationsFromJson(QJsonArray{json}).first().concurrency == 4,
            "missing limit defaults to four");
    json.insert(QStringLiteral("concurrency"), 1.5);
    require(!storage::ConfigurationSchema::normalize(key, QJsonArray{json}).valid,
            "fraction rejected");
    const QMap<QString, QJsonValue> snapshot{{key, textTranslationConfigurationsToJson({value})}};
    const auto archive = directory.filePath(QStringLiteral("export.zip"));
    require(storage::ConfigurationArchive::write(archive, snapshot, 3, true).isEmpty(),
            "redacted export");
    auto imported = storage::ConfigurationArchive::read(archive);
    require(
        imported.isValid() &&
            textTranslationConfigurationsFromJson(imported.values[key]).first().apiKey.isEmpty(),
        "secret omitted");
    imported.preserveOmittedCredentials(snapshot);
    require(textTranslationConfigurationsFromJson(imported.values[key]).first() == value,
            "same connection secret restored");
    auto changed = value;
    changed.endpoint += QStringLiteral("&other=1");
    imported = storage::ConfigurationArchive::read(archive);
    imported.preserveOmittedCredentials({{key, textTranslationConfigurationsToJson({changed})}});
    require(textTranslationConfigurationsFromJson(imported.values[key]).first().apiKey.isEmpty(),
            "changed endpoint cannot inherit secret");
}
void protocols() {
    for (const auto& provider :
         {QStringLiteral("deepl"), QStringLiteral("baidu"), QStringLiteral("youdao")}) {
        HttpServer server;
        SnowShotApiClient client(QString{});
        auto value = config(server.url(), provider);
        client.setTextTranslationConfigurations({value});
        QObject receiver;
        SnowShotTranslationResult result;
        QString text;
        bool done = false;
        const QString source = QString::fromUtf8("Hello + & \xE4\xBD\xA0\xE5\xA5\xBD");
        const auto token = client.streamTranslation(
            {value.selectionId(), QStringLiteral("auto"), QStringLiteral("zh-Hant"), source,
             QStringLiteral("default"), QStringLiteral("ja")},
            &receiver, [&](const QString& delta) { text += delta; },
            [&](auto response) {
                result = response;
                done = true;
            });
        require(token != 0 && !done, "async token returned");
        waitUntil([&] { return server.requests.size() == 1; }, "provider receives request");
        const auto request = server.requests.first();
        require(request.headers.startsWith("POST /custom/translate?route=alpha%2Bbeta HTTP/1.1"),
                "exact custom path and query");
        if (provider == QStringLiteral("deepl")) {
            require(request.headers.contains("DeepL-Auth-Key secret"), "DeepL authorization");
            const auto body = QJsonDocument::fromJson(request.body).object();
            require(!body.contains(QStringLiteral("source_lang")) &&
                        body[QStringLiteral("target_lang")] == QStringLiteral("ZH-HANT"),
                    "DeepL auto and Traditional Chinese");
            require(body[QStringLiteral("text")].toArray().first() == source, "DeepL text");
            server.respond(0);
        } else {
            QUrlQuery form(QString::fromUtf8(request.body));
            const auto field = [&](const QString& name) {
                return form.queryItemValue(name, QUrl::FullyDecoded);
            };
            require(field(QStringLiteral("q")) == source,
                    "form preserves Unicode plus and ampersand");
            const auto salt = field(QStringLiteral("salt"));
            require(!salt.isEmpty(), "salt present");
            const auto preimage =
                value.applicationId + source + salt +
                (provider == QStringLiteral("youdao") ? field(QStringLiteral("curtime"))
                                                      : QString()) +
                value.apiKey;
            const auto expected =
                QCryptographicHash::hash(preimage.toUtf8(), provider == QStringLiteral("youdao")
                                                                ? QCryptographicHash::Sha256
                                                                : QCryptographicHash::Md5)
                    .toHex();
            require(field(QStringLiteral("sign")).toLatin1() == expected, "standard signature");
            if (provider == QStringLiteral("baidu")) {
                require(field(QStringLiteral("to")) == QStringLiteral("cht"), "Baidu language");
                server.respond(0, "{\"trans_result\":[{\"dst\":\"translated\"}]}");
            } else {
                require(field(QStringLiteral("to")) == QStringLiteral("zh-CHT") &&
                            field(QStringLiteral("signType")) == QStringLiteral("v3"),
                        "Youdao language and v3");
                server.respond(0, "{\"errorCode\":\"0\",\"translation\":[\"translated\"]}");
            }
        }
        waitUntil([&] { return done; }, "provider finishes");
        require(result.succeeded() && text == QStringLiteral("translated"),
                "nonstreaming provider integrates result");
        done = false;
        require(
            client.streamTranslation(
                {value.selectionId(), QStringLiteral("auto"), QStringLiteral("invalid"), source},
                &receiver, [](const auto&) {},
                [&](auto r) {
                    result = r;
                    done = true;
                }) != 0,
            "unsupported language returns async error");
        waitUntil([&] { return done; }, "language failure");
        require(!result.succeeded() && server.requests.size() == 1,
                "unsupported language never sent");
    }
    auto value = config(QStringLiteral("http://host/api"), QStringLiteral("youdao"));
    const QString longText = QString::fromUcs4(U"0123456789abcdefghij\U0001F600");
    QUrlQuery form(QString::fromUtf8(text_translation::body(
        value, {value.selectionId(), QStringLiteral("en"), QStringLiteral("de"), longText},
        QStringLiteral("salt"), QStringLiteral("123"))));
    const QString input = QString::fromUcs4(U"012345678921bcdefghij\U0001F600");
    require(form.queryItemValue(QStringLiteral("sign")) ==
                QString::fromLatin1(
                    QCryptographicHash::hash(
                        (QStringLiteral("app") + input + QStringLiteral("salt123secret")).toUtf8(),
                        QCryptographicHash::Sha256)
                        .toHex()),
            "Youdao long text signs Unicode code points");
}
void providerFailures() {
    struct Failure {
        QString provider;
        QByteArray body;
        int status;
        int expectedStatus;
    };
    const QList<Failure> cases{
        {QStringLiteral("deepl"), QByteArrayLiteral(R"({"message":"quota exhausted"})"), 456, 456},
        {QStringLiteral("deepl"), QByteArrayLiteral(R"({"translations":[]})"), 200, 200},
        {QStringLiteral("deepl"), QByteArrayLiteral(R"({"message":"throttled"})"), 429, 429},
        {QStringLiteral("baidu"), QByteArrayLiteral(R"({"error_code":"54001"})"), 200, 200},
        {QStringLiteral("youdao"), QByteArrayLiteral(R"({"errorCode":"411"})"), 200, 429},
        {QStringLiteral("youdao"), QByteArrayLiteral(R"({"errorCode":"202"})"), 200, 200},
        {QStringLiteral("youdao"), QByteArrayLiteral(R"({"errorCode":"0","translation":[42]})"),
         200, 200}};
    for (const auto& failure : cases) {
        HttpServer server;
        SnowShotApiClient client(QString{});
        const auto value = config(server.url(), failure.provider);
        client.setTextTranslationConfigurations({value});
        QObject receiver;
        bool done = false;
        SnowShotTranslationResult result;
        require(
            client.streamTranslation(
                {value.selectionId(), QStringLiteral("auto"), QStringLiteral("en"),
                 QStringLiteral("text")},
                &receiver,
                [](const auto&) { require(false, "failure must not publish translation text"); },
                [&](auto response) {
                    result = response;
                    done = true;
                }) != 0,
            "failure request");
        waitUntil([&] { return server.requests.size() == 1; }, "failure request arrives");
        server.respond(0, failure.body, failure.status);
        waitUntil([&] { return done; }, "failure delivered");
        require(!result.succeeded() && result.httpStatus == failure.expectedStatus &&
                    SnowShotApiClientTestAccess::requests(client) == 0,
                "provider failure is normalized and releases capacity");
    }
}

void queueAndJobs() {
    for (int limit : {1, 4, 16}) {
        HttpServer server;
        QTemporaryDir directory;
        storage::ConfigurationStore settings(directory.filePath(QStringLiteral("config.json")),
                                             true, true, 8000);
        auto value = config(server.url());
        value.concurrency = limit;
        require(settings.setValue(key, textTranslationConfigurationsToJson({value})),
                "configure jobs");
        SnowShotApiClient client(QString{});
        auto& service =
            translation::TranslationService::forClient(client, settings, QLocale(QLocale::English));
        require(service.models().size() == 1 &&
                    service.preferences().modelId == value.selectionId(),
                "local provider selected without server catalog");
        require(client.fallbackModel(true).isEmpty(), "excluded from vision");
        QStringList texts;
        for (int i = 0; i < 20; ++i)
            texts.append(QString::number(i));
        std::unique_ptr<translation::TranslationJob> first(service.createJob(texts, nullptr));
        std::unique_ptr<translation::TranslationJob> second(service.createJob(texts, nullptr));
        first->start();
        second->start();
        waitUntil([&] { return server.requests.size() == limit; }, "parallel requests start");
        require(SnowShotApiClientTestAccess::requests(client) -
                        SnowShotApiClientTestAccess::queued(client) ==
                    limit,
                "per-config ceiling shared across jobs");
        value.name = QStringLiteral("Renamed");
        require(settings.setValue(key, textTranslationConfigurationsToJson({value})), "rename");
        require(first->busy() && second->busy() &&
                    service.preferences().modelId == value.selectionId(),
                "rename preserves jobs and selection");
        first->cancel();
        second->cancel();
        flushEvents();
        require(SnowShotApiClientTestAccess::requests(client) == 0 &&
                    SnowShotApiClientTestAccess::queued(client) == 0,
                "cancel clears active and queued requests");
    }
    HttpServer server;
    SnowShotApiClient client(QString{});
    auto value = config(server.url());
    value.concurrency = 1;
    client.setTextTranslationConfigurations({value});
    QObject receiver;
    int completed = 0;
    QList<SnowShotApiClient::RequestToken> tokens;
    for (int i = 0; i < 5; ++i)
        tokens.append(client.streamTranslation(
            {value.selectionId(), QStringLiteral("auto"), QStringLiteral("en"), QString::number(i)},
            &receiver, [](const auto&) {},
            [&](auto r) {
                require(r.succeeded(), "queued result");
                ++completed;
            }));
    waitUntil([&] { return server.requests.size() == 1; }, "one starts");
    client.cancel(tokens[1]);
    server.respond(0);
    waitUntil([&] { return server.requests.size() == 2; }, "cancel skips queue item");
    require(QJsonDocument::fromJson(server.requests[1].body)
                    .object()[QStringLiteral("text")]
                    .toArray()
                    .first() == QStringLiteral("2"),
            "FIFO");
    value.concurrency = 3;
    client.setTextTranslationConfigurations({value});
    waitUntil([&] { return server.requests.size() == 4; }, "increase fills capacity");
    value.concurrency = 1;
    client.setTextTranslationConfigurations({value});
    require(SnowShotApiClientTestAccess::requests(client) == 3, "decrease retains active work");
    server.respond(1);
    server.respond(2);
    server.respond(3);
    waitUntil([&] { return completed == 4; }, "remaining requests finish");
    auto owner = std::make_unique<QObject>();
    for (int i = 0; i < 3; ++i)
        require(client.streamTranslation(
                    {value.selectionId(), QStringLiteral("auto"), QStringLiteral("en"),
                     QStringLiteral("owner")},
                    owner.get(), [](const auto&) {},
                    [](auto) { require(false, "destroyed owner has no callbacks"); }) != 0,
                "owner request");
    waitUntil([&] { return server.requests.size() == 5; }, "owner starts");
    owner.reset();
    flushEvents();
    require(SnowShotApiClientTestAccess::requests(client) == 0,
            "owner destruction clears queue and active");
}
void unavailableCatalog() {
    Server builtIn;
    builtIn.rejectModels = true;
    HttpServer custom;
    QTemporaryDir directory;
    storage::ConfigurationStore settings(directory.filePath(QStringLiteral("config.json")), true,
                                         true, 8000);
    const auto value = config(custom.url());
    require(settings.setValue(key, textTranslationConfigurationsToJson({value})),
            "custom settings before catalog");
    SnowShotApiClient client(builtIn.url());
    auto& service =
        translation::TranslationService::forClient(client, settings, QLocale(QLocale::English));
    service.refreshModels();
    waitUntil([&] { return !service.loadingModels(); }, "failed built-in catalog returns");
    require(service.models().size() == 1 && service.preferences().modelId == value.selectionId() &&
                service.errorText().isEmpty(),
            "custom service survives unavailable built-in catalog");
}

void independentQueuesAndReentrancy() {
    HttpServer firstServer;
    HttpServer secondServer;
    SnowShotApiClient client(QString{});
    auto first = config(firstServer.url());
    first.concurrency = 1;
    auto second = config(secondServer.url());
    second.name = QStringLiteral("Second");
    second.concurrency = 4;
    client.setTextTranslationConfigurations({first, second});
    QObject receiver;
    int completed = 0;
    for (const auto& value : {first, second}) {
        for (int index = 0; index < 6; ++index)
            require(client.streamTranslation(
                        {value.selectionId(), QStringLiteral("auto"), QStringLiteral("en"),
                         QString::number(index)},
                        &receiver, [](const auto&) {}, [&](auto) { ++completed; }) != 0,
                    "independent queue request");
    }
    waitUntil([&] { return firstServer.requests.size() == 1 && secondServer.requests.size() == 4; },
              "independent configuration limits");
    first.concurrency = 2;
    client.setTextTranslationConfigurations({first, second});
    waitUntil([&] { return firstServer.requests.size() == 2; }, "increase only first queue");
    first.concurrency = 1;
    client.setTextTranslationConfigurations({first, second});
    firstServer.respond(0);
    waitUntil([&] { return completed == 1; }, "one outstanding request after decrease");
    flushEvents();
    require(firstServer.requests.size() == 2, "decrease prevents admission while at capacity");
    firstServer.respond(1);
    waitUntil([&] { return firstServer.requests.size() == 3; },
              "queue resumes below decreased cap");
    client.setTextTranslationConfigurations({});
    flushEvents();
    require(SnowShotApiClientTestAccess::requests(client) == 0 &&
                SnowShotApiClientTestAccess::queued(client) == 0,
            "deletion cancels both queues");

    HttpServer server;
    auto owned = std::make_unique<SnowShotApiClient>(QString{});
    auto value = config(server.url());
    owned->setTextTranslationConfigurations({value});
    require(owned->streamTranslation(
                {value.selectionId(), QStringLiteral("auto"), QStringLiteral("en"),
                 QStringLiteral("test")},
                &receiver, [&](const auto&) { owned.reset(); },
                [](auto) { require(false, "no completion after delta destroys client"); }) != 0,
            "reentrant request");
    waitUntil([&] { return server.requests.size() == 1; }, "reentrant request arrives");
    server.respond(0);
    waitUntil([&] { return !owned; }, "consumer may destroy client during result callback");
}

void errorsAndInvalidation() {
    HttpServer server;
    SnowShotApiClient client(QString{});
    auto value = config(server.url(), QStringLiteral("baidu"));
    client.setTextTranslationConfigurations({value});
    QObject receiver;
    int completed = 0;
    SnowShotTranslationResult result;
    const auto submit = [&] {
        require(client.streamTranslation(
                    {value.selectionId(), QStringLiteral("auto"), QStringLiteral("en"),
                     QStringLiteral("test")},
                    &receiver, [](const auto&) {},
                    [&](auto r) {
                        result = r;
                        ++completed;
                    }) != 0,
                "error request");
    };
    submit();
    waitUntil([&] { return server.requests.size() == 1; }, "error arrives");
    server.respond(0, "{\"error_code\":\"54003\",\"error_msg\":\"rate limited\"}");
    waitUntil([&] { return completed == 1; }, "error completion");
    require(result.httpStatus == 429 && !result.succeeded(), "provider throttle normalized");
    submit();
    waitUntil([&] { return server.requests.size() == 2; }, "malformed arrives");
    server.respond(1, "not json");
    waitUntil([&] { return completed == 2; }, "malformed completion");
    require(!result.succeeded(), "malformed fails");
    submit();
    waitUntil([&] { return server.requests.size() == 3; }, "timeout arrives");
    SnowShotApiClientTestAccess::timeout(client);
    waitUntil([&] { return completed == 3; }, "timeout completion");
    require(!result.succeeded() && SnowShotApiClientTestAccess::requests(client) == 0,
            "timeout frees slot");
    QTemporaryDir directory;
    storage::ConfigurationStore settings(directory.filePath(QStringLiteral("config.json")), true,
                                         true, 8000);
    require(settings.setValue(key, textTranslationConfigurationsToJson({value})),
            "service settings");
    auto& service =
        translation::TranslationService::forClient(client, settings, QLocale(QLocale::English));
    std::unique_ptr<translation::TranslationJob> job(
        service.createJob({QStringLiteral("text")}, nullptr));
    job->start();
    waitUntil([&] { return server.requests.size() == 4; }, "job starts");
    value.apiKey = QStringLiteral("changed");
    require(settings.setValue(key, textTranslationConfigurationsToJson({value})),
            "edit credentials");
    require(job->state() == translation::TranslationJob::State::Invalidated,
            "connection edit invalidates job");
    require(settings.setValue(key, QJsonArray{}), "delete configuration");
    require(service.models().isEmpty() && service.preferences().modelId.isEmpty(),
            "delete clears stale selection");
    value.concurrency = 1;
    require(settings.setValue(key, textTranslationConfigurationsToJson({value})),
            "restore provider");
    std::unique_ptr<translation::TranslationJob> throttled(service.createJob(
        {QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}, nullptr));
    throttled->start();
    waitUntil([&] { return server.requests.size() == 5; }, "throttled job starts");
    server.respond(4, "{\"error_code\":\"54003\"}");
    waitUntil([&] { return !throttled->busy(); }, "throttled job stops");
    require(SnowShotApiClientTestAccess::requests(client) == 0 && server.requests.size() == 5,
            "throttling cancels remaining queued job work");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    persistence();
    protocols();
    providerFailures();
    queueAndJobs();
    unavailableCatalog();
    independentQueuesAndReentrancy();
    errorsAndInvalidation();
    return 0;
}
