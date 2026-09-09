#include "translation_test_support.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/storage/configurationstore.h"
#include <QTemporaryDir>
#include <memory>

using namespace translation_tests;
using namespace snow_shot::translation;
using snow_shot::storage::ConfigurationStore;

namespace {
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
    service.refreshModels();
    service.refreshModels();
    QObject owner;
    auto* first = service.createJob({QStringLiteral("same input")}, &owner);
    auto* second = service.createJob({QStringLiteral("same input")}, &owner);
    first->start();
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
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "isolated translation service settings");
    customModelsStayAvailableAndInvalidateTogether(directory.path());
    failuresAndOwnerLifetimes(directory.path());
    return 0;
}
