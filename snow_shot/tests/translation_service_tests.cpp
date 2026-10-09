#include "translation_test_support.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/translation/translationlanguages.h"
#include <QTemporaryDir>
#include <memory>

using namespace translation_tests;
using namespace snow_shot::translation;
using snow_shot::storage::ConfigurationStore;

namespace {
void discoverySharesRequestsAndCancellation(const QString& directory) {
    Server server;
    server.holdModels = true;
    SnowShotApiClient client(server.url());
    auto settings = std::make_unique<ConfigurationStore>(
        directory + QStringLiteral("/shared-discovery.json"), true, true, 60000);
    QPointer<TranslationService> service =
        &TranslationService::forClient(client, *settings, QLocale::English);
    const QString locale = QLocale(QLocale::English).name();
    QObject recognitionReceiver;
    int cancelledCompletions = 0;
    const auto recognition = client.ensureChatModels(locale, &recognitionReceiver,
                                                     [&](auto) { ++cancelledCompletions; });
    require(recognition != 0, "recognition starts shared model discovery");
    service->refreshModels();
    service->refreshModels();
    waitUntil([&] { return server.modelRequests == 1; }, "translation joins recognition discovery");
    client.cancel(recognition);
    server.respondModels();
    waitUntil([&] { return !service->loadingModels(); },
              "translation finishes after recognition subscriber cancellation");
    require(server.modelRequests == 1 && cancelledCompletions == 0 &&
                service->preferences().modelId == QStringLiteral("general") &&
                service->models() == client.cachedChatModels(),
            "translation and recognition share one independently cancellable catalog request");

    service->refreshModels();
    waitUntil([&] { return !service->loadingModels(); }, "translation reuses the cached catalog");
    require(server.modelRequests == 1, "ordinary translation discovery avoids another request");

    server.models =
        QJsonArray{QJsonObject{{QStringLiteral("model"), QStringLiteral("updated")},
                               {QStringLiteral("name"), QStringLiteral("Updated model")},
                               {QStringLiteral("supports_vision"), true}}};
    const auto refresh = client.ensureChatModels(
        locale, &recognitionReceiver, [&](auto) { ++cancelledCompletions; },
        SnowShotApiClient::ChatModelsCachePolicy::Refresh);
    require(refresh != 0, "recognition explicitly refreshes the cached catalog");
    service->refreshModels();
    require(service->loadingModels(), "ordinary translation discovery joins an in-flight refresh");
    waitUntil([&] { return server.modelRequests == 2; }, "refresh sends one shared request");
    client.cancel(refresh);
    server.respondModels();
    waitUntil([&] { return !service->loadingModels(); }, "translation receives refreshed models");
    require(server.modelRequests == 2 && cancelledCompletions == 0 &&
                service->preferences().modelId == QStringLiteral("updated"),
            "shared refresh updates translation selection after the other subscriber cancels");

    service->refreshModels(true);
    int refreshCompletions = 0;
    require(client.ensureChatModels(
                locale, &recognitionReceiver,
                [&](SnowShotChatModelsResult result) {
                    require(result.succeeded() &&
                                result.models.first().id == QStringLiteral("updated"),
                            "remaining recognition subscriber receives refreshed models");
                    ++refreshCompletions;
                },
                SnowShotApiClient::ChatModelsCachePolicy::Refresh) != 0,
            "recognition joins explicit translation refresh");
    waitUntil([&] { return server.modelRequests == 3; },
              "explicit translation refresh bypasses the warm cache once");
    settings.reset();
    require(service == nullptr, "settings destruction removes the translation subscriber");
    server.respondModels();
    waitUntil([&] { return refreshCompletions == 1; },
              "recognition discovery survives translation subscriber destruction");
    require(server.modelRequests == 3 && cancelledCompletions == 0,
            "explicit refresh and cancellation preserve a single catalog owner");
}

void secondaryTargetsPersistAndReachEveryUnit(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/secondary.json"), true, true, 60000);
    require(settings.setValue(QStringLiteral("screenshot_translation/target_language"),
                              QStringLiteral("zh-Hant")),
            "save an existing primary target");
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    require(service.preferences().targetLanguage == QStringLiteral("zh-Hant") &&
                service.preferences().secondaryTargetLanguage == QStringLiteral("en"),
            "new secondary default preserves an existing primary selection");
    for (const auto& language : translationLanguages()) {
        const QString code = QString::fromLatin1(language.code);
        require(
            settings.setValue(QStringLiteral("screenshot_translation/source_language"), code) &&
                settings.setValue(QStringLiteral("screenshot_translation/target_language"), code) &&
                settings.setValue(
                    QStringLiteral("screenshot_translation/secondary_target_language"), code),
            "primary and secondary accept the entire language catalog, including identical "
            "targets");
    }
    require(!settings.setValue(QStringLiteral("screenshot_translation/secondary_target_language"),
                               QStringLiteral("auto")),
            "secondary target cannot auto-detect");
    auto preferences = service.preferences();
    preferences.sourceLanguage = QStringLiteral("auto");
    preferences.targetLanguage = QStringLiteral("zh-Hans");
    preferences.secondaryTargetLanguage = QStringLiteral("ja");
    preferences.modelId = QStringLiteral("general");
    require(service.savePreferences(preferences), "save shared secondary preferences");
    QObject owner;
    auto* job = service.createJob({QStringLiteral("中文"), QStringLiteral("Hello")}, &owner);
    job->start();
    waitUntil([&] { return server.streams.size() == 2; }, "each input block has its own request");
    for (const auto& stream : server.streams) {
        const auto messages = stream.body.value(QStringLiteral("messages")).toArray();
        const auto prompt = messages.first().toObject().value(QStringLiteral("content")).toString();
        require(prompt.contains(QStringLiteral("Primary target language: zh-Hans")) &&
                    prompt.contains(QStringLiteral("Secondary target language: ja")) &&
                    prompt.contains(QStringLiteral("independent text block")),
                "every unit receives both shared targets and independent routing instructions");
    }
    job->cancel();
    require(settings.flushNow().success, "persist secondary preference before reopening");
    ConfigurationStore reopened(directory + QStringLiteral("/secondary.json"), true, true, 60000);
    require(reopened.value(QStringLiteral("screenshot_translation/secondary_target_language")) ==
                QStringLiteral("ja"),
            "secondary preference survives storage reload");
}

void customModelsStayAvailableAndInvalidateTogether(const QString& directory) {
    Server builtIn;
    builtIn.holdModels = true;
    Server custom;
    custom.streamPath = QByteArrayLiteral("/v1/chat/completions");
    snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Local model"),
        custom.url() + QStringLiteral("/v1"),
        QStringLiteral("test-key"),
        QStringLiteral("provider-model"),
        true};
    ConfigurationStore settings(directory + QStringLiteral("/custom.json"), true, true, 60000);
    const QString key = QStringLiteral("api_configuration/custom_models");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "save custom configuration");
    SnowShotApiClient client(builtIn.url());
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    require(&service == &TranslationService::forClient(client, settings, QLocale::English),
            "consumers share the catalog and settings owner");
    require(service.models().size() == 1 && service.models().first().supportsVision &&
                service.preferences().modelId == model.selectionId(),
            "custom vision model is immediately eligible");
    {
        QObject fixedOwner;
        auto preferences = service.preferences();
        preferences.sourceLanguage = QStringLiteral("en");
        preferences.targetLanguage = QStringLiteral("ja");
        auto* fixed = service.createJob({QStringLiteral(" ")}, preferences, &fixedOwner);
        const auto global = service.preferences();
        fixed->start();
        fixed->retry();
        require(
            fixed->preferences() == preferences && service.preferences() == global,
            "explicit per-job preferences survive start/retry without changing global settings");
    }
    service.refreshModels();
    service.refreshModels();
    QObject owner;
    auto* first = service.createJob({QStringLiteral("same input")}, &owner);
    auto* second = service.createJob({QStringLiteral("same input")}, &owner);
    first->start();
    // Identical parallel requests can reach the server in either order. Establish the
    // first socket's owner before starting the second, while retaining overlapping jobs.
    waitUntil([&] { return custom.streams.size() == 1 && builtIn.modelRequests == 1; },
              "first custom stream does not wait for builtin discovery");
    second->start();
    waitUntil([&] { return custom.streams.size() == 2 && builtIn.modelRequests == 1; },
              "custom streams do not wait for coalesced builtin discovery");
    require(custom.streams[0].body == custom.streams[1].body &&
                custom.streams[0].body.value(QStringLiteral("model")) == model.model &&
                custom.streams[0].headers.contains("Authorization: Bearer test-key") &&
                !custom.streams[0].headers.contains(model.selectionId().toUtf8()),
            "consumers produce identical provider requests with scoped custom credentials");
    custom.delta(0, QStringLiteral("first output"));
    custom.delta(1, QStringLiteral("second output"));
    waitUntil(
        [&] {
            return !first->units().first().text.isEmpty() &&
                   !second->units().first().text.isEmpty();
        },
        "both jobs receive text");
    model.name = QStringLiteral("Renamed model");
    model.supportsVision = false;
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "rename and change vision capability");
    require(first->busy() && second->busy() && service.models().first().name == model.name,
            "display metadata edits preserve text translation");
    custom.finish(0);
    waitUntil([&] { return !first->busy(); }, "cache one completed result");
    model.apiKey = QStringLiteral("updated-key");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "edit connection settings");
    waitUntil([&] { return custom.disconnected(1); }, "connection edit aborts affected stream");
    require(first->state() == TranslationJob::State::Invalidated &&
                second->state() == TranslationJob::State::Invalidated &&
                first->units().first().text.isEmpty() && second->units().first().text.isEmpty() &&
                custom.streams.size() == 2,
            "completed and running results invalidate without automatic restart");
    second->retry();
    waitUntil([&] { return custom.streams.size() == 3; },
              "explicit retry starts updated connection");
    require(custom.streams.last().headers.contains("Authorization: Bearer updated-key"),
            "retry uses new credentials");
    builtIn.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "finish builtin discovery");
    require(settings.setValue(key, QJsonArray{}), "delete selected custom model");
    require(second->state() == TranslationJob::State::Invalidated &&
                service.preferences().modelId == QStringLiteral("general") &&
                custom.streams.size() == 3 && builtIn.streams.isEmpty(),
            "deletion selects shared fallback without sending another translation");
}

void failuresAndOwnerLifetimes(const QString& directory) {
    Server server;
    ConfigurationStore settings(directory + QStringLiteral("/lifecycle.json"), true, true, 60000);
    auto client = std::make_unique<SnowShotApiClient>(server.url());
    auto& service = TranslationService::forClient(*client, settings, QLocale::English);
    QObject owner;
    auto* empty = service.createJob({QStringLiteral("   ")}, &owner);
    empty->start();
    require(empty->state() == TranslationJob::State::Completed && server.modelRequests == 0,
            "empty source does not perform discovery");
    auto* job = service.createJob({QStringLiteral("hello")}, &owner);
    job->start();
    waitUntil([&] { return server.streams.size() == 1; }, "prepare nonempty input");
    server.finish(0);
    waitUntil([&] { return !job->busy(); }, "empty response finishes");
    require(job->state() == TranslationJob::State::Failed && !job->errorText().isEmpty(),
            "empty model response is an actionable error");
    job->retry();
    waitUntil([&] { return server.streams.size() == 2; }, "retry empty response");
    server.delta(1, QStringLiteral("partial"));
    waitUntil([&] { return !job->units().first().text.isEmpty(); }, "deliver partial response");
    server.fail(1);
    waitUntil([&] { return !job->busy(); }, "fail partial response");
    require(job->units().first().text == QStringLiteral("partial"),
            "ordinary errors retain partial output");
    job->retry();
    waitUntil([&] { return server.streams.size() == 3; }, "start before client destruction");
    client.reset();
    require(!job->busy() && !job->errorText().isEmpty(),
            "provider destruction settles the job safely");
}
void serverChangesRefreshModelsAndPreserveStreams(const QString& directory) {
    Server first, second, custom;
    second.holdModels = true;
    custom.streamPath = QByteArrayLiteral("/v1/chat/completions");
    ConfigurationStore settings(directory + QStringLiteral("/servers.json"), true, true, 60000);
    const QString serverKey = QStringLiteral("api_configuration/server_url");
    const snow_shot::CustomAiModelConfiguration customModel{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Custom"),
        custom.url() + QStringLiteral("/v1"),
        QStringLiteral("test-key"),
        QStringLiteral("provider-model"),
        true};
    require(settings.setValue(QStringLiteral("api_configuration/custom_models"),
                              snow_shot::customAiModelsToJson({customModel})),
            "save custom model");
    SnowShotApiClient client(first.url());
    auto& service = TranslationService::forClient(client, settings, QLocale::English);
    service.refreshModels();
    waitUntil([&] { return !service.loadingModels(); }, "initial server catalog loaded");
    auto preferences = service.preferences();
    preferences.modelId = QStringLiteral("vision");
    require(service.savePreferences(preferences), "select model that exists on both servers");
    QObject receiver;
    bool originalDone = false;
    SnowShotTranslationRequest input;
    input.model = QStringLiteral("general");
    input.text = QStringLiteral("hello");
    require(client.streamTranslation(
                input, &receiver, [](const QString&) {}, [&](auto) { originalDone = true; }) != 0,
            "start original server stream");
    waitUntil([&] { return first.streams.size() == 1; }, "original stream is active");
    require(settings.setValue(serverKey, second.url()), "save replacement server");
    waitUntil([&] { return second.modelRequests == 1; }, "new server catalog refresh starts");
    require(service.loadingModels() && service.preferences().modelId == QStringLiteral("vision") &&
                !client.hasBuiltInModels(QLocale(QLocale::English).name()),
            "old catalog is cleared without prematurely replacing selected model");
    first.delta(0, QStringLiteral("old output"));
    first.finish(0);
    waitUntil([&] { return originalDone; }, "old stream finishes after switch");
    second.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "replacement catalog loaded");
    require(service.preferences().modelId == QStringLiteral("vision"),
            "shared selected model survives server change");
    require(client.streamTranslation(
                input, &receiver, [](const QString&) {}, [](auto) {}) != 0,
            "new built-in stream starts");
    waitUntil([&] { return second.streams.size() == 1; }, "new stream reaches replacement server");
    input.model = customModel.selectionId();
    require(client.streamTranslation(
                input, &receiver, [](const QString&) {}, [](auto) {}) != 0,
            "custom stream starts");
    waitUntil([&] { return custom.streams.size() == 1; }, "custom provider retains own address");
    second.finish(0);
    custom.finish(0);

    // A second switch while discovery is pending must retire the first refresh.
    first.holdModels = true;
    require(settings.setValue(serverKey, first.url()), "start another held discovery");
    waitUntil([&] { return first.modelRequests == 2; }, "discovery is in flight");
    second.models =
        QJsonArray{QJsonObject{{QStringLiteral("model"), QStringLiteral("replacement")},
                               {QStringLiteral("name"), QStringLiteral("Replacement")}}};
    require(settings.setValue(serverKey, second.url()), "switch during discovery");
    waitUntil([&] { return second.modelRequests == 2; }, "latest discovery is in flight");
    second.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "latest discovery finishes");
    first.respondModels();
    flushEvents();
    require(service.preferences().modelId != QStringLiteral("vision") &&
                client.baseUrl() == second.url() &&
                client.cachedChatModels().first().id == QStringLiteral("replacement"),
            "missing selection falls back and retired refresh cannot overwrite latest catalog");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "isolated translation service settings");
    discoverySharesRequestsAndCancellation(directory.path());
    secondaryTargetsPersistAndReachEveryUnit(directory.path());
    customModelsStayAvailableAndInvalidateTogether(directory.path());
    failuresAndOwnerLifetimes(directory.path());
    serverChangesRefreshModelsAndPreserveStreams(directory.path());
    return 0;
}
