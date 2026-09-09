#include "translation_test_support.h"

#include "snow_shot/presentation/translationlanguages.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/configurationstore.h"

#include <QTemporaryDir>

#include <memory>

using namespace translation_tests;
using snow_shot::presentation::TranslationPageController;
using snow_shot::storage::ConfigurationStore;

namespace {
void sharedLanguageAndModelHelpers() {
    using namespace snow_shot::presentation;
    require(translationLanguages().size() == 12 &&
                defaultTranslationTargetLanguage(QLocale(QStringLiteral("zh_TW"))) ==
                    QStringLiteral("zh-Hant") &&
                defaultTranslationTargetLanguage(QLocale(QStringLiteral("ja_JP"))) ==
                    QStringLiteral("ja") &&
                defaultTranslationTargetLanguage(QLocale(QStringLiteral("ko_KR"))) ==
                    QStringLiteral("en"),
            "shared language metadata retains supported locales and fallback");
    const QVector<SnowShotChatModel> models{
        {QStringLiteral("vision"), QStringLiteral("Vision"), false, QStringLiteral("default"),
         true},
        {QStringLiteral("specialist"), QStringLiteral("Specialist"), false,
         QStringLiteral("qwen-mt")},
        {QStringLiteral("general"), QStringLiteral("General"), false, QStringLiteral("default")}};
    require(translationModelIndex(models, QStringLiteral("specialist")) == 1 &&
                translationModelIndex(models, QStringLiteral("missing")) == 2 &&
                translationModelIndex(models, QStringLiteral("vision")) == 2 &&
                translationModelIndex(models.mid(0, 2), {}) == 1 &&
                translationModelIndex(models.mid(0, 1), {}) == -1,
            "eligible services prefer saved, general, then translation, excluding vision");
    auto customModels = models;
    auto customVision = models.first();
    customVision.id = QStringLiteral("custom:vision");
    customVision.origin = SnowShotModelOrigin::Custom;
    customModels.append(customVision);
    require(translationModelIndex(customModels, customVision.id) == 3 &&
                translationModelIndex({customVision}, {}) == 0,
            "custom vision models remain eligible for saved and fallback translation selection");
}

void coalescedEditsAndCancellation(const QString& directory) {
    Server server;
    server.holdModels = true;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/cancellation.json"), true, true,
                                60000);
    require(settings.setValue(QStringLiteral("screenshot_translation/model"),
                              QStringLiteral("specialist")),
            "save a preferred service before discovery");
    auto controller =
        std::make_unique<TranslationPageController>(client, settings, QLocale::English, nullptr, 0);
    controller->activate();
    controller->setSourceText(QStringLiteral("pending"));
    controller->setSourceText({});
    require(controller->setPreferences(QStringLiteral("en"), QStringLiteral("ja"),
                                       controller->preferences().modelId),
            "edit languages while discovery is pending");
    waitUntil([&]() { return server.modelRequests == 1; }, "hold discovery while edits coalesce");
    server.respondModels();
    waitUntil([&]() { return !controller->loadingModels(); }, "finish held discovery");
    require(server.streams.isEmpty() &&
                controller->preferences().modelId == QStringLiteral("specialist"),
            "clearing a pending draft sends nothing and language edits preserve the saved service");
    controller->setSourceText(QStringLiteral("first edit"));
    controller->setSourceText(QStringLiteral("latest edit"));
    waitUntil([&]() { return server.streams.size() == 1; },
              "one request for coalesced source edits");
    require(server.streams[0]
                    .body.value(QStringLiteral("messages"))
                    .toArray()
                    .last()
                    .toObject()
                    .value(QStringLiteral("content"))
                    .toString() == QStringLiteral("latest edit"),
            "only the final edit reaches the server");
    bool atomicSwap = true;
    const auto connection = QObject::connect(
        &settings, &ConfigurationStore::valueChanged, &settings,
        [&](const QString&, const QJsonValue&) {
            atomicSwap = atomicSwap &&
                         settings.value(QStringLiteral("screenshot_translation/source_language")) ==
                             QStringLiteral("ja") &&
                         settings.value(QStringLiteral("screenshot_translation/target_language")) ==
                             QStringLiteral("en");
        });
    controller->swapLanguages();
    QObject::disconnect(connection);
    waitUntil([&]() { return server.streams.size() == 2; },
              "atomic swap starts just one translation");
    require(atomicSwap && controller->sourceText() == QStringLiteral("latest edit"),
            "swap observers see both committed languages and unchanged source text");
    server.delta(1, QStringLiteral("queued stale text"));
    server.finish(1);
    controller->setSourceText({});
    flushEvents();
    flushEvents();
    require(controller->resultText().isEmpty() && !controller->translating() &&
                controller->errorText().isEmpty() && server.streams.size() == 2,
            "clearing an active request rejects queued deltas and completion");
    controller->setSourceText(QStringLiteral("last request"));
    waitUntil([&]() { return server.streams.size() == 3; },
              "start a stream before destroying its page");
    server.delta(2, QStringLiteral("queued before destruction"));
    controller.reset();
    waitUntil([&]() { return server.disconnected(2); }, "destruction cancels streaming safely");
}

void streamLifecycleAndSharedPreferences(const QString& directory) {
    Server server;
    server.holdModels = true;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/stream.json"), true, true, 60000);
    TranslationPageController controller(client, settings, QLocale(QStringLiteral("zh_CN")),
                                         nullptr, 0);
    require(controller.preferences().sourceLanguage == QStringLiteral("auto") &&
                controller.preferences().targetLanguage == QStringLiteral("zh-Hans"),
            "fresh preferences use auto source and application locale target");
    controller.activate();
    controller.setSourceText(QStringLiteral("obsolete"));
    controller.setSourceText(QStringLiteral("Hello\n\nworld"));
    waitUntil([&]() { return server.modelRequests == 1; }, "request model catalog");
    require(server.streams.isEmpty(), "translation waits for model discovery");
    server.respondModels();
    waitUntil([&]() { return server.streams.size() == 1; }, "translate latest debounced source");
    require(server.streams[0].body.value(QStringLiteral("model")).toString() ==
                QStringLiteral("general"),
            "default selection excludes vision and prefers general");
    const auto messages = server.streams[0].body.value(QStringLiteral("messages")).toArray();
    require(messages.last().toObject().value(QStringLiteral("content")).toString() ==
                QStringLiteral("Hello\n\nworld"),
            "source formatting is preserved verbatim");
    server.delta(0, QStringLiteral("你好\n\n"));
    waitUntil([&]() { return !controller.resultText().isEmpty(); }, "display first streamed delta");
    require(controller.translating(), "partial result remains in streaming state");
    require(
        settings.setValues(
            {{QStringLiteral("screenshot_translation/source_language"), QStringLiteral("en")},
             {QStringLiteral("screenshot_translation/target_language"), QStringLiteral("zh-Hant")},
             {QStringLiteral("screenshot_translation/model"), QStringLiteral("specialist")}}),
        "commit shared preferences");
    waitUntil([&]() { return server.streams.size() == 2; },
              "one restart for atomic settings update");
    require(controller.resultText().isEmpty() &&
                controller.preferences().targetLanguage == QStringLiteral("zh-Hant"),
            "shared changes clear obsolete output");
    waitUntil([&]() { return server.disconnected(0); }, "cancel superseded streaming request");
    require(server.streams[1].body.contains(QStringLiteral("translation_options")),
            "specialist service retains provider-specific request mapping");
    server.delta(1, QStringLiteral("世界"));
    waitUntil([&]() { return controller.resultText() == QStringLiteral("世界"); },
              "new stream wins");
    server.fail(1);
    waitUntil([&]() { return !controller.errorText().isEmpty(); }, "surface stream failure");
    require(!controller.translating() && controller.resultText() == QStringLiteral("世界"),
            "failure preserves copyable partial text separately from error");
    controller.retry();
    waitUntil([&]() { return server.streams.size() == 3; }, "explicit retry starts a new request");
    server.delta(2, QStringLiteral("完整的翻譯"));
    server.finish(2);
    waitUntil([&]() { return !controller.translating(); }, "stream finishes");
    require(controller.errorText().isEmpty() &&
                controller.resultText() == QStringLiteral("完整的翻譯"),
            "successful retry replaces partial output and clears failure");
    controller.setLocale(QLocale(QStringLiteral("en_US")));
    flushEvents();
    require(server.streams.size() == 3 && !controller.resultText().isEmpty(),
            "UI locale changes preserve the current translation");
    controller.setSourceText(QStringLiteral("   \n"));
    flushEvents();
    require(controller.resultText().isEmpty() && server.streams.size() == 3,
            "whitespace clears the output without a request");
    controller.setComposing(true);
    controller.setSourceText(QStringLiteral("composing"));
    flushEvents();
    require(server.streams.size() == 3, "IME preedit never reaches the service");
    controller.setComposing(false);
    waitUntil([&]() { return server.streams.size() == 4; }, "translate committed IME text");
    controller.deactivate();
    waitUntil([&]() { return server.disconnected(3); }, "deactivation aborts active stream");
    require(controller.sourceText().isEmpty() && controller.resultText().isEmpty(),
            "deactivation discards the page draft");
}

void modelFailureAndDestroyedReceiver(const QString& directory) {
    Server server;
    server.rejectModels = true;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/models.json"), true, true, 60000);
    TranslationPageController controller(client, settings, QLocale::English, nullptr, 0);
    controller.activate();
    controller.setSourceText(QStringLiteral("Hello"));
    waitUntil([&]() { return !controller.errorText().isEmpty(); }, "model error is actionable");
    require(server.streams.isEmpty(), "failed discovery cannot start a translation");
    server.rejectModels = false;
    server.models = QJsonArray{server.models.first()};
    controller.retry();
    waitUntil([&]() { return server.modelRequests == 2 && !controller.loadingModels(); },
              "retry discovery with a vision-only catalog");
    require(!controller.errorText().isEmpty() && server.streams.isEmpty(),
            "vision-only catalog is unavailable for translation");
    controller.deactivate();
    server.holdModels = true;
    SnowShotApiClient pendingClient(server.url());
    auto* pending =
        new TranslationPageController(pendingClient, settings, QLocale::English, nullptr, 0);
    pending->activate();
    waitUntil([&]() { return server.modelRequests == 3; },
              "hold model response for destroyed page");
    delete pending;
    flushEvents();
    server.respondModels();
    flushEvents();
    require(server.streams.isEmpty(), "destroyed model receiver cannot start work");
}

void persistenceAndFailedWrites(const QString& directory) {
    const QString file = directory + QStringLiteral("/preferences.json");
    SnowShotApiClient client(QString{});
    {
        ConfigurationStore settings(file, true, true, 60000);
        require(settings.setValue(QStringLiteral("screenshot_translation/layout_processing"),
                                  QStringLiteral("original")),
                "set screenshot-only setting");
        TranslationPageController controller(client, settings, QLocale::English);
        require(controller.setPreferences(QStringLiteral("en"), QStringLiteral("ja"),
                                          QStringLiteral("general")),
                "save shared preferences");
        controller.swapLanguages();
        require(controller.preferences().sourceLanguage == QStringLiteral("ja") &&
                    controller.preferences().targetLanguage == QStringLiteral("en") &&
                    settings.value(QStringLiteral("screenshot_translation/layout_processing")) ==
                        QStringLiteral("original"),
                "swap only languages, preserving screenshot settings");
        require(settings.flushNow().success, "persist shared preferences");
    }
    ConfigurationStore settings(file, true, false, 60000);
    TranslationPageController controller(client, settings, QLocale::English);
    require(controller.preferences().sourceLanguage == QStringLiteral("ja"),
            "shared selections survive restart");
    require(!controller.setPreferences(QStringLiteral("auto"), QStringLiteral("fr"),
                                       QStringLiteral("specialist")) &&
                controller.preferences().sourceLanguage == QStringLiteral("ja") &&
                !controller.errorText().isEmpty(),
            "failed save restores committed preferences");
}

void independentConsumers(const QString& directory) {
    Server server;
    SnowShotApiClient client(server.url());
    ConfigurationStore settings(directory + QStringLiteral("/independent.json"), true, true, 60000);
    TranslationPageController first(client, settings, QLocale::English, nullptr, 0);
    TranslationPageController second(client, settings, QLocale::English, nullptr, 0);
    first.activate();
    first.setSourceText(QStringLiteral("first"));
    waitUntil([&] { return server.streams.size() == 1; }, "first consumer starts");
    second.activate();
    second.setSourceText(QStringLiteral("second"));
    waitUntil([&] { return server.streams.size() == 2; }, "second consumer starts");
    server.delta(0, QStringLiteral("first output"));
    server.delta(1, QStringLiteral("second output"));
    waitUntil([&] { return !first.resultText().isEmpty() && !second.resultText().isEmpty(); },
              "both consumers receive independent output");
    first.deactivate();
    waitUntil([&] { return server.disconnected(0); }, "closing one consumer cancels its stream");
    require(!server.disconnected(1) && second.translating(),
            "closing one consumer leaves the other running");
    server.finish(1);
    waitUntil([&] { return !second.translating(); }, "remaining consumer finishes");
    require(first.resultText().isEmpty() && second.resultText() == QStringLiteral("second output"),
            "consumer results never mix");
}

void customModelEditStopsPendingDraft(const QString& directory) {
    Server server;
    server.streamPath = QByteArrayLiteral("/v1/chat/completions");
    ConfigurationStore settings(directory + QStringLiteral("/pending-custom.json"), true, true,
                                60000);
    snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("33333333-3333-4333-8333-333333333333"),
        QStringLiteral("Custom model"),
        server.url() + QStringLiteral("/v1"),
        {},
        QStringLiteral("provider-model"),
        true};
    const QString key = QStringLiteral("api_configuration/custom_models");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "configure the pending draft's custom model");
    SnowShotApiClient client(QString{});
    TranslationPageController controller(client, settings, QLocale::English, nullptr, 60000);
    controller.activate();
    controller.setSourceText(QStringLiteral("pending draft"));
    model.apiKey = QStringLiteral("changed");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "edit the model before the debounce expires");
    require(!controller.translating() && !controller.errorText().isEmpty() &&
                server.streams.isEmpty(),
            "pending drafts require explicit retry after a model edit");
    controller.retry();
    waitUntil([&] { return server.streams.size() == 1; },
              "explicit retry submits the pending draft");
    require(server.streams.first().headers.contains("Authorization: Bearer changed"),
            "the pending draft uses the updated configuration");
    model.apiKey = QStringLiteral("changed-again");
    require(settings.setValue(key, snow_shot::customAiModelsToJson({model})),
            "invalidate the retried draft");
    controller.deactivate();
    controller.activate();
    require(controller.sourceText().isEmpty() && controller.errorText().isEmpty(),
            "reopening an empty page does not retain the discarded draft's retry error");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "create test storage");
    sharedLanguageAndModelHelpers();
    coalescedEditsAndCancellation(directory.path());
    streamLifecycleAndSharedPreferences(directory.path());
    modelFailureAndDestroyedReceiver(directory.path());
    persistenceAndFailedWrites(directory.path());
    independentConsumers(directory.path());
    customModelEditStopsPendingDraft(directory.path());
    return 0;
}
