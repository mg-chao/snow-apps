#include "translation_test_support.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/translation/freetranslation.h"
#include "snow_shot/translation/translationlanguages.h"
#include "snow_shot/translation/translationservice.h"
#include "../src/network/freetranslationprotocol.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <memory>

using namespace translation_tests;
using namespace snow_shot::translation;
using snow_shot::storage::ConfigurationStore;

class SnowShotApiClientTestAccess {
  public:
    static void endpoint(SnowShotApiClient& client, const QString& url) {
        client.m_freeTranslationBaseUrl = url;
    }
    static qsizetype requests(const SnowShotApiClient& client) {
        return client.m_requests.size();
    }
    static void timeout(SnowShotApiClient& client) {
        for (auto* timer : client.findChildren<QTimer*>())
            if (timer->isActive())
                timer->setInterval(1);
    }
};

namespace {
const QString modelKey = QStringLiteral("screenshot_translation/model");
const QString message = QStringLiteral(
    "The free translation service may no longer be available. Please update the app or report "
    "the issue");

SnowShotTranslationRequest request() {
    return {freeTranslationId, QStringLiteral("auto"), QStringLiteral("zh-Hant"),
            QStringLiteral("Hello\nworld")};
}

void configure(Server& server, SnowShotApiClient& client) {
    server.streamPath = QByteArrayLiteral("/v1/chat/completions");
    SnowShotApiClientTestAccess::endpoint(client, server.url() + QStringLiteral("/v1"));
}

void requestAndStreaming() {
    Server server;
    SnowShotApiClient client{QString()};
    configure(server, client);
    QObject owner;
    QString output;
    int completions = 0;
    require(client.streamTranslation(
                request(), &owner, [&](const QString& text) { output += text; },
                [&](auto result) {
                    require(result.succeeded(), "free stream succeeds");
                    ++completions;
                }) != 0,
            "free service works without a Snow Shot server");
    waitUntil([&] { return server.streams.size() == 1; }, "public endpoint receives request");
    const auto& stream = server.streams.first();
    const auto body = stream.body;
    const auto messages = body.value(QStringLiteral("messages")).toArray();
    const QString prompt = messages.first().toObject().value(QStringLiteral("content")).toString();
    require(body.value(QStringLiteral("model")) == QStringLiteral("Index-Translate-35B-A3B") &&
                body.value(QStringLiteral("stream")).toBool() &&
                body.value(QStringLiteral("temperature")).toInt(-1) == 0 &&
                body.value(QStringLiteral("max_tokens")).toInt() == 4096 &&
                body.value(QStringLiteral("chat_template_kwargs"))
                        .toObject()
                        .value(QStringLiteral("enable_thinking")) == QJsonValue(false) &&
                messages.size() == 1 &&
                messages.first().toObject().value(QStringLiteral("role")) ==
                    QStringLiteral("user") &&
                prompt.contains(QStringLiteral("Traditional Chinese")) &&
                prompt.endsWith(request().text) && !prompt.contains(QStringLiteral("auto")),
            "request follows public API contract and distinguishes Chinese scripts");
    require(!stream.headers.toLower().contains("authorization:") &&
                stream.headers.contains("User-Agent: Index-Translate-Client/1.0") &&
                server.modelRequests == 0,
            "public service requires neither credentials nor model discovery");
    server.send(0, "data: {\"choices\":[]}\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"");
    server.send(0, QStringLiteral("你好").toUtf8().left(2));
    flushEvents();
    server.send(0, QStringLiteral("你好").toUtf8().mid(2) + "\"}}]}\r\n\r\n");
    server.send(0, "data: {\"usage\":{}}\n\ndata: "
                   "{\"choices\":[{\"delta\":null,\"finish_reason\":\"stop\"}]}\n\n");
    server.finish(0);
    waitUntil([&] { return completions == 1; }, "fragmented UTF-8 stream finishes once");
    require(output == QStringLiteral("你好") && SnowShotApiClientTestAccess::requests(client) == 0,
            "stream delivers text and releases requests");
    for (const auto& language : translationLanguages()) {
        auto input = request();
        input.sourceLanguage = QStringLiteral("en");
        input.targetLanguage = QString::fromLatin1(language.code);
        const auto translatedPrompt = snow_shot::network::freeTranslationBody(input)
                                          .value(QStringLiteral("messages"))
                                          .toArray()
                                          .first()
                                          .toObject()
                                          .value(QStringLiteral("content"))
                                          .toString();
        require(translatedPrompt.contains(QString::fromLatin1(language.name)) &&
                    translatedPrompt.contains(QStringLiteral("English text")),
                "every existing language has a stable protocol name");
    }
    const auto fingerprint = client.modelFingerprint(freeTranslationId);
    require(client.setBaseUrl(QStringLiteral("http://127.0.0.1:1")) &&
                client.modelFingerprint(freeTranslationId) == fingerprint,
            "free service identity does not depend on the Snow Shot server");
}

void failuresAndCancellation() {
    for (int scenario = 0; scenario < 12; ++scenario) {
        Server server;
        SnowShotApiClient client{QString()};
        configure(server, client);
        if (scenario == 0 || scenario == 1) {
            server.streamRejection = [scenario](int) {
                const QByteArray body = "{\"error\":\"unavailable\"}";
                return QByteArray("HTTP/1.1 ") +
                       (scenario == 0 ? "503 Unavailable" : "429 Limited") +
                       "\r\nContent-Type: application/json\r\nContent-Length: " +
                       QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            };
        }
        QObject owner;
        int completed = 0;
        SnowShotTranslationResult result;
        require(client.streamTranslation(
                    request(), &owner, [](const QString&) {},
                    [&](auto value) {
                        result = value;
                        ++completed;
                    }) != 0,
                "error fixture starts asynchronously");
        waitUntil([&] { return server.streams.size() == 1; }, "error fixture receives request");
        switch (scenario) {
        case 0:
        case 1:
            break;
        case 2:
            server.fail(0);
            break;
        case 3:
            server.send(0, "data: {\"error\":\"service removed\"}\n\n");
            break;
        case 4:
            server.send(0, "data: invalid json\n\n");
            break;
        case 5:
            server.delta(0, QStringLiteral("partial"));
            server.streams[0].socket->disconnectFromHost();
            break;
        case 6:
            server.finish(0);
            break;
        case 7:
            SnowShotApiClientTestAccess::timeout(client);
            break;
        case 8:
            server.send(0, "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n");
            break;
        case 9:
            server.delta(0, QStringLiteral("partial"));
            server.send(0, "data: {\"choices\":[{\"delta\":42}]}\n\n");
            break;
        case 10:
            server.delta(0, QStringLiteral("partial"));
            server.send(0, "data: [DONE]\n\ngarbage");
            server.streams[0].socket->disconnectFromHost();
            break;
        case 11:
            server.streams[0].socket->abort();
            break;
        }
        waitUntil([&] { return completed == 1; }, "free service failure completes once");
        require(result.error == message && !result.succeeded() && server.streams.size() == 1 &&
                    SnowShotApiClientTestAccess::requests(client) == 0,
                "all failures show the exact message, without automatic retry");
    }
    for (const bool destroyOwner : {false, true}) {
        Server server;
        SnowShotApiClient client{QString()};
        configure(server, client);
        auto owner = std::make_unique<QObject>();
        int completed = 0;
        auto token = client.streamTranslation(
            request(), owner.get(), [](const QString&) {}, [&](auto) { ++completed; });
        waitUntil([&] { return server.streams.size() == 1; }, "cancellable request starts");
        if (destroyOwner)
            owner.reset();
        else
            client.cancel(token);
        waitUntil([&] { return server.disconnected(0); }, "cancellation releases connection");
        require(completed == 0 && SnowShotApiClientTestAccess::requests(client) == 0,
                "cancellation is silent and releases request state");
    }
}

void migration(const QString& directory) {
    const QString path = directory + QStringLiteral("/migration.json");
    const QStringList selections{
        QString(), QStringLiteral("ai-model"), QStringLiteral("qwen-mt-flash"),
        QStringLiteral("custom:11111111-1111-4111-8111-111111111111"),
        QStringLiteral("translation:22222222-2222-4222-8222-222222222222")};
    for (int version = 1; version <= 6; ++version) {
        for (const auto& selected : selections) {
            const auto bytes =
                QJsonDocument(
                    QJsonObject{{QStringLiteral("storage"),
                                 QJsonObject{{QStringLiteral("schema_version"), version}}},
                                {QStringLiteral("screenshot_translation"),
                                 QJsonObject{{QStringLiteral("model"), selected}}}})
                    .toJson();
            {
                QFile file(path);
                require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                        "write versioned settings fixture");
            }
            const bool migrate = version < 5 && !selected.startsWith(u"custom:") &&
                                 !selected.startsWith(u"translation:");
            const QString expected = migrate ? freeTranslationId : selected;
            {
                ConfigurationStore settings(path, true, true, 60000);
                require(settings.value(modelKey) == expected,
                        "only older built-in and empty selections are replaced");
                require(settings.flushNow().success, "flush migration");
                if (version == 6) {
                    QFile file(path);
                    require(file.open(QIODevice::ReadOnly) && file.readAll() == bytes &&
                                !settings.isWritable(),
                            "future settings stay untouched");
                }
            }
            ConfigurationStore reloaded(path, true, true, 60000);
            require(reloaded.value(modelKey) == expected && !reloaded.isDirty(),
                    "migration persists and is idempotent");
            if (version < 6) {
                require(reloaded.setValue(modelKey, QStringLiteral("chosen-later")) &&
                            reloaded.flushNow().success,
                        "choose another provider after upgrade");
                ConfigurationStore again(path, true, true, 60000);
                require(again.value(modelKey) == QStringLiteral("chosen-later"),
                        "later choices survive reload");
                require(reloaded.applySnapshot({{modelKey, selected}}, version) &&
                            reloaded.value(modelKey) == expected,
                        "imports use the same migration");
            }
        }
    }
    ConfigurationStore fresh(directory + QStringLiteral("/fresh.json"), true, true, 60000);
    require(fresh.value(modelKey) == freeTranslationId &&
                fresh.setValue(modelKey, QStringLiteral("ai-model")) && fresh.applySnapshot({}) &&
                fresh.value(modelKey) == freeTranslationId,
            "new installs and resets default to Free Translation");
    require(fresh.applySnapshot({}, 4) && fresh.value(modelKey) == freeTranslationId,
            "older imports missing a selection use the new default");
    const snow_shot::CustomAiModelConfiguration custom{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Custom"),
        QStringLiteral("http://127.0.0.1:1/v1"),
        {},
        QStringLiteral("custom-model"),
        false};
    const snow_shot::TextTranslationConfiguration configured{
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        QStringLiteral("Configured"),
        QStringLiteral("deeplx"),
        QStringLiteral("http://127.0.0.1:1/translate"),
        {},
        {}};
    for (const auto& selected : {custom.selectionId(), configured.selectionId()}) {
        require(
            fresh.applySnapshot({{modelKey, selected},
                                 {QStringLiteral("api_configuration/custom_models"),
                                  snow_shot::customAiModelsToJson({custom})},
                                 {QStringLiteral("api_configuration/text_translation"),
                                  snow_shot::textTranslationConfigurationsToJson({configured})}},
                                4),
            "import older settings with both user-configured provider types");
        SnowShotApiClient client{QString()};
        auto& service = TranslationService::forClient(client, fresh, QLocale::English);
        require(service.preferences().modelId == selected && service.models().size() == 3,
                "initial catalog publication preserves the selected configured provider");
    }
}

void boundedQueueAndQueuedCancellation() {
    Server server;
    SnowShotApiClient client{QString()};
    configure(server, client);
    auto owner = std::make_unique<QObject>();
    QVector<SnowShotApiClient::RequestToken> tokens;
    int completed = 0;
    for (int i = 0; i < 10; ++i) {
        auto input = request();
        input.text = QString::number(i);
        tokens.append(client.streamTranslation(
            input, owner.get(), [](const QString&) {},
            [&](auto result) {
                require(result.succeeded(), "queued request succeeds");
                ++completed;
            }));
    }
    waitUntil([&] { return server.streams.size() == 8; }, "free service fills eight shared slots");
    require(client.isQueued(tokens[8]) && client.isQueued(tokens[9]),
            "excess free requests wait in the bounded queue");
    client.cancel(tokens[8]);
    server.delta(0, QStringLiteral("done"));
    server.finish(0);
    waitUntil([&] { return server.streams.size() == 9; }, "a completion admits queued work");
    require(server.streams.last()
                .body.value(QStringLiteral("messages"))
                .toArray()
                .first()
                .toObject()
                .value(QStringLiteral("content"))
                .toString()
                .endsWith(u"\n\n9"),
            "a cancelled queued input never reaches the provider");
    owner.reset();
    require(completed == 1 && SnowShotApiClientTestAccess::requests(client) == 0,
            "owner destruction cancels all remaining free requests");
}

class Translator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char*, const char* source, const char*, int) const override {
        if (QString::fromUtf8(source) == u"Free Translation")
            return QStringLiteral("Localized free translation");
        if (QString::fromUtf8(source) == message)
            return QStringLiteral("Localized failure");
        return {};
    }
};

void localCatalogAndRetry(const QString& directory) {
    Server catalog;
    catalog.rejectModels = true;
    Server free;
    SnowShotApiClient client(catalog.url());
    configure(free, client);
    ConfigurationStore settings(directory + QStringLiteral("/catalog.json"), true, true, 60000);
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    require(service.models().size() == 1 && service.models().first().id == freeTranslationId &&
                !service.models().first().supportsVision && client.cachedChatModels().isEmpty(),
            "free option is locally available only to translation consumers");
    service.refreshModels();
    QObject owner;
    auto* job = service.createJob({QStringLiteral("Hello")}, &owner);
    job->start();
    waitUntil([&] { return free.streams.size() == 1 && !service.loadingModels(); },
              "translation runs despite catalog failure");
    require(service.errorText().isEmpty() && service.preferences().modelId == freeTranslationId,
            "catalog failure does not invalidate the free option");
    free.fail(0);
    waitUntil([&] { return job->state() == TranslationJob::State::Failed; }, "job reports failure");
    require(job->errorText() == message, "job shows the exact availability message");
    Translator translator;
    QCoreApplication::installTranslator(&translator);
    service.setLocale(QLocale::German);
    require(service.models().first().name == u"Localized free translation" &&
                job->errorText() == u"Localized failure",
            "cached label and error retranslate");
    QCoreApplication::removeTranslator(&translator);
    job->retry();
    waitUntil([&] { return free.streams.size() == 2; }, "manual retry uses the same provider");
    free.delta(1, QStringLiteral("translated"));
    free.finish(1);
    waitUntil([&] { return job->state() == TranslationJob::State::Completed; }, "retry completes");
    require(job->errorText().isEmpty() && service.preferences().modelId == freeTranslationId,
            "successful retry clears the error without changing selection");
    require(settings.setValue(modelKey, QStringLiteral("custom:removed")) &&
                settings.setValue(
                    QStringLiteral("api_configuration/custom_models"),
                    snow_shot::customAiModelsToJson({snow_shot::CustomAiModelConfiguration{
                        QStringLiteral("33333333-3333-4333-8333-333333333333"),
                        QStringLiteral("Other custom model"),
                        catalog.url(),
                        {},
                        QStringLiteral("custom-model"),
                        false}})) &&
                service.preferences().modelId == freeTranslationId,
            "missing custom selection falls back to Free Translation");
    const int index = translationModelIndex(service.models(), freeTranslationId);
    require(index > 0 && service.models()[index - 1].translationMode == u"default" &&
                service.models()[index].translationMode != u"default",
            "free option leads the translation group after general models");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "isolated free translation fixtures");
    requestAndStreaming();
    failuresAndCancellation();
    boundedQueueAndQueuedCancellation();
    migration(directory.path());
    localCatalogAndRetry(directory.path());
    return 0;
}
