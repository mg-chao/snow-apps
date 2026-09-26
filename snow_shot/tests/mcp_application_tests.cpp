#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationarchive.h"
#include "../src/app/mcp/mcpsettingsadapter_p.h"
#include "translation_test_support.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/update/updateservice.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot;
using namespace snow_shot::app::mcp;
namespace settings = presentation::settings;
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

void runMcpApplicationTests() {
    auto& storage = storage::ApplicationStorage::instance();
    auto& configuration = storage.configuration();
    presentation::GlobalShortcutManager shortcuts;
    const auto suspended = shortcuts.suspendRegistrations();
    Q_UNUSED(suspended);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    McpJobRegistry jobs;
    QTemporaryDir updateDirectory;
    update::UpdateService::Options updateOptions;
    updateOptions.applicationDirectory = updateDirectory.filePath(QStringLiteral("missing-helper"));
    updateOptions.root = updateDirectory.path();
    updateOptions.cacheDirectory = updateDirectory.filePath(QStringLiteral("cache"));
    // macOS rejects non-HTTPS before network access; Windows lacks its packaged helper here.
    updateOptions.baseUrl = QUrl(QStringLiteral("http://updates.example.invalid"));
    update::UpdateService updates(std::move(updateOptions));
    translation_tests::Server provider;
    SnowShotApiClient api(provider.url());
    auto& translation =
        translation::TranslationService::forClient(api, configuration, QLocale::English);
    McpApplicationService::Ports ports;
    ports.storage = &storage;
    ports.settings = &session;
    ports.jobs = &jobs;
    ports.translation = &translation;
    ports.updates = &updates;
    int lifecycleActions = 0;
    ports.restartAllowed = [] { return true; };
    ports.action = [&](const QString&, const QJsonObject&) {
        ++lifecycleActions;
        return true;
    };
    McpApplicationService service(ports);
    quint64 sequence = 0;
    const auto call = [&](const QString& method, QJsonObject params = {},
                          std::optional<quint64> revision = {}, QString key = {},
                          quint64 owner = 71) {
        ScreenshotMcpRequest request;
        request.connectionId = owner;
        request.requestId = QString::number(++sequence);
        request.method = QStringLiteral("snow_shot_") + method;
        request.params = std::move(params);
        request.expectedRevision = revision;
        request.idempotencyKey = std::move(key);
        std::optional<ScreenshotMcpResponse> result;
        service.request(request, [&](auto response) { result = std::move(response); });
        QElapsedTimer timer;
        timer.start();
        while (!result && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(result.has_value(), "application request completes");
        return *result;
    };
    const auto snapshot = call(QStringLiteral("settings_get"));
    require(snapshot.ok && snapshot.result.value(QStringLiteral("fields")).toArray().size() ==
                               registry.fields().size() + 1,
            "every catalog field has discoverable runtime state");
    require(!QJsonDocument(snapshot.result).toJson().contains("api_key"),
            "settings discovery never exposes credential keys");
    // Verify each editable configuration field has a lossless public representation.
    for (const auto& field : registry.fields()) {
        if (field.kind == settings::SettingsFieldKind::Action ||
            field.kind == settings::SettingsFieldKind::Custom || field.configurationKey.isEmpty())
            continue;
        const auto state = session.state(field.id);
        if (!state.acceptedValue.isValid())
            continue;
        QVariant decoded;
        if (!settingsValue(field, settingsJson(state.acceptedValue), &decoded)) {
            std::cerr << "setting round trip failed: " << field.id.toStdString() << '\n';
            std::exit(1);
        }
    }
    const auto* field = registry.field(QStringLiteral("quick.screenshot-delay"));
    require(field != nullptr, "delay shortcut field registered");
    const auto auxiliary = auxiliaryIntegerSetting(*field);
    require(auxiliary.has_value(), "delay adjustment discoverable");
    const auto initialRevision = configuration.revision();
    const int delay = session.integerValue(auxiliary->binding) == 3 ? 4 : 3;
    const QJsonObject patch{{QStringLiteral("values"), QJsonObject{{auxiliary->id, delay}}}};
    const auto changed = call(QStringLiteral("settings_update"), patch, initialRevision,
                              QStringLiteral("delay-change"));
    require(changed.ok && configuration.value(auxiliary->configurationKey).toInt() == delay &&
                session.integerValue(auxiliary->binding) == delay,
            "MCP setting commits through the same runtime session as UI");
    const auto committed = configuration.revision();
    require(committed > initialRevision, "settings mutation advances revision");
    require(call(QStringLiteral("settings_update"), patch, initialRevision,
                 QStringLiteral("delay-change"))
                    .ok &&
                configuration.revision() == committed,
            "idempotency replays result without a second mutation");
    require(call(QStringLiteral("settings_update"), patch, initialRevision).errorCode ==
                QStringLiteral("stale_revision"),
            "stale setting writes rejected");
    require(call(QStringLiteral("settings_update"), {}, committed, QStringLiteral("delay-change"))
                    .errorCode == QStringLiteral("idempotency_conflict"),
            "reused token with different arguments rejected");
    require(jobs.list(71).isEmpty(), "synchronous settings patches release reserved job capacity");
    const auto historyField =
        std::find_if(registry.fields().cbegin(), registry.fields().cend(), [](const auto& item) {
            return item.configurationKey == u"capture_history/enabled";
        });
    require(historyField != registry.fields().cend(), "history runtime setting exists");
    const auto policyWrite =
        call(QStringLiteral("settings_update"),
             {{QStringLiteral("values"),
               QJsonObject{
                   {historyField->id, !session.state(historyField->id).acceptedValue.toBool()}}}},
             configuration.revision());
    const auto policyJob = policyWrite.result.value(QStringLiteral("job_id")).toString();
    require(policyWrite.ok && !policyJob.isEmpty() &&
                policyWrite.result.value(QStringLiteral("pending_fields"))
                    .toArray()
                    .contains(historyField->id) &&
                !policyWrite.result.value(QStringLiteral("applied_fields"))
                     .toArray()
                     .contains(historyField->id),
            "asynchronous runtime writes are pending until their service commits");
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, policyJob)->value(QStringLiteral("status")) !=
                   QStringLiteral("running");
        },
        "pending settings job completes from runtime field signals");
    require(jobs.get(71, policyJob)->value(QStringLiteral("status")) ==
                    QStringLiteral("completed") &&
                !session.state(historyField->id).busy,
            "history policy job reports the committed runtime value");
    const auto policyReset = call(QStringLiteral("settings_reset"),
                                  {{QStringLiteral("page_id"), historyField->pageId},
                                   {QStringLiteral("section_id"), historyField->sectionId}},
                                  configuration.revision());
    require(policyReset.ok, "runtime reset accepted");
    if (const auto id = policyReset.result.value(QStringLiteral("job_id")).toString();
        !id.isEmpty()) {
        translation_tests::waitUntil(
            [&] {
                return jobs.get(71, id)->value(QStringLiteral("status")) !=
                       QStringLiteral("running");
            },
            "asynchronous reset commits before its job completes");
        require(jobs.get(71, id)->value(QStringLiteral("status")) == QStringLiteral("completed"),
                "reset job observes successful runtime completion");
    }
    CustomAiModelConfiguration model;
    model.id = QStringLiteral("73444af2-eed2-4195-872a-92b3b04f0ef1");
    model.name = QStringLiteral("Private provider");
    model.baseUrl = QStringLiteral("https://example.invalid/v1");
    model.model = QStringLiteral("test-model");
    model.apiKey = QStringLiteral("test-secret-never-returned");
    require(session.applyCustomAiModels({model}), "prepare existing model");
    const auto models = call(QStringLiteral("models_list"));
    require(models.ok && !QJsonDocument(models.result).toJson().contains(model.apiKey.toUtf8()),
            "model discovery redacts secrets");
    auto publicModel = publicModels({model}).first().toObject();
    publicModel.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
    require(call(QStringLiteral("models_update"),
                 {{QStringLiteral("models"), QJsonArray{publicModel}}}, configuration.revision())
                    .ok &&
                session.customAiModels().first().apiKey == model.apiKey,
            "omitted credential preserved for same provider endpoint");
    publicModel.insert(QStringLiteral("base_url"), QStringLiteral("https://different.invalid/v1"));
    require(call(QStringLiteral("models_update"),
                 {{QStringLiteral("models"), QJsonArray{publicModel}}}, configuration.revision())
                    .ok &&
                session.customAiModels().first().apiKey.isEmpty(),
            "omitted secret is not transferred to a new endpoint");
    auto accepted =
        call(QStringLiteral("app_action"), {{QStringLiteral("action"), QStringLiteral("quit")}}, {},
             QStringLiteral("quit-once"));
    require(accepted.ok && accepted.afterSend && lifecycleActions == 0,
            "shutdown action waits for transport acknowledgment");
    accepted.afterSend();
    auto repeated =
        call(QStringLiteral("app_action"), {{QStringLiteral("action"), QStringLiteral("quit")}}, {},
             QStringLiteral("quit-once"));
    require(repeated.ok && !repeated.afterSend && lifecycleActions == 1,
            "shutdown replay never repeats accepted action");
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("redacted.zip"));
    require(call(QStringLiteral("configuration_export"), {{QStringLiteral("path"), path}}).ok,
            "configuration export uses bounded worker");
    const auto archive = storage::ConfigurationArchive::read(path);
    require(archive.isValid() && archive.redactedCredentialIds.contains(model.id),
            "configuration archive records redacted provider identities");
    const auto archivedTheme =
        archive.values.value(QStringLiteral("interface/theme_mode")).toString();
    require(session.applySelectValue(settings::SettingsSelectBinding::Theme,
                                     archivedTheme == u"dark" ? QStringLiteral("light")
                                                              : QStringLiteral("dark")),
            "set a different runtime theme before import");
    const bool archivedHistoryEnabled =
        archive.values.value(QStringLiteral("capture_history/enabled")).toBool();
    require(session.applySwitchValue(settings::SettingsSwitchBinding::HistoryEnabled,
                                     !archivedHistoryEnabled),
            "set a different asynchronous history policy before import");
    translation_tests::waitUntil([&] { return !storage.status().historyPolicyUpdating; },
                                 "pre-import history policy commits");
    const auto imported = call(QStringLiteral("configuration_import"),
                               {{QStringLiteral("path"), path}}, configuration.revision());
    require(imported.ok, "configuration import accepts an owned job");
    const auto jobId = imported.result.value(QStringLiteral("job_id")).toString();
    require(jobs.get(71, jobId).has_value(), "configuration job belongs to caller");
    QElapsedTimer importTimer;
    importTimer.start();
    while (jobs.get(71, jobId)->value(QStringLiteral("status")) == QStringLiteral("running") &&
           importTimer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(jobs.get(71, jobId)->value(QStringLiteral("status")) == QStringLiteral("completed") &&
                session.selectValue(settings::SettingsSelectBinding::Theme).toString() ==
                    archivedTheme &&
                !storage.status().historyPolicyUpdating &&
                session.switchValue(settings::SettingsSwitchBinding::HistoryEnabled) ==
                    archivedHistoryEnabled,
            "configuration import waits for runtime theme and asynchronous history policy");
    require(call(QStringLiteral("history_list"), {{QStringLiteral("limit"), 0}}).errorCode ==
                QStringLiteral("invalid_parameters"),
            "history page limits enforced");
    require(
        call(QStringLiteral("history_list"), {{QStringLiteral("cursor"), QStringLiteral("bogus")}})
                .errorCode == QStringLiteral("invalid_cursor"),
        "history cursor validated");
    require(call(QStringLiteral("translation_catalog")).ok, "translation catalog available");
    translation_tests::waitUntil([&] { return !translation.loadingModels(); },
                                 "deterministic provider catalog completes");
    const auto started = call(QStringLiteral("translation_start"),
                              {{QStringLiteral("texts"), QJsonArray{QStringLiteral("hello")}},
                               {QStringLiteral("model_id"), QStringLiteral("general")},
                               {QStringLiteral("source_language"), QStringLiteral("en")},
                               {QStringLiteral("target_language"), QStringLiteral("ja")}});
    require(started.ok, "translation returns an owned job");
    const auto translationId = started.result.value(QStringLiteral("job_id")).toString();
    translation_tests::waitUntil([&] { return provider.streams.size() == 1; },
                                 "translation uses deterministic provider");
    require(
        call(QStringLiteral("translation_start"), {{QStringLiteral("retry_job_id"), translationId}})
                .errorCode == QStringLiteral("invalid_state"),
        "cannot retry active translation");
    provider.delta(0, QStringLiteral("partial"));
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, translationId)
                       ->value(QStringLiteral("progress"))
                       .toObject()
                       .value(QStringLiteral("text")) == QStringLiteral("partial");
        },
        "streamed translation progress is observable");
    provider.fail(0);
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, translationId)->value(QStringLiteral("status")) ==
                   QStringLiteral("failed");
        },
        "provider failure produces terminal job");
    const auto snapshotInput = jobs.input(71, translationId);
    require(snapshotInput.has_value() && !jobs.input(72, translationId),
            "retained retry input checks ownership");
    require(call(QStringLiteral("translation_start"),
                 {{QStringLiteral("retry_job_id"), translationId}}, {}, {}, 72)
                    .errorCode == QStringLiteral("job_not_found"),
            "wrong-owner retry does not disclose private job");
    auto changedPreferences = translation.preferences();
    changedPreferences.targetLanguage = QStringLiteral("de");
    require(translation.savePreferences(changedPreferences), "UI changes translation preferences");
    const auto retried = call(QStringLiteral("translation_start"),
                              {{QStringLiteral("retry_job_id"), translationId}});
    require(retried.ok, "failed translation can retry");
    const auto retryId = retried.result.value(QStringLiteral("job_id")).toString();
    require(retryId != translationId && jobs.input(71, retryId) == snapshotInput,
            "retry owns a new handle and preserves captured preferences");
    translation_tests::waitUntil([&] { return provider.streams.size() == 2; },
                                 "retry reaches provider");
    provider.delta(1, QStringLiteral("translated"));
    provider.finish(1);
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, retryId)->value(QStringLiteral("status")) ==
                   QStringLiteral("completed");
        },
        "retry completes");
    const auto canceled =
        call(QStringLiteral("translation_start"), {{QStringLiteral("retry_job_id"), retryId}});
    const auto canceledId = canceled.result.value(QStringLiteral("job_id")).toString();
    translation_tests::waitUntil([&] { return provider.streams.size() == 3; },
                                 "cancel fixture starts");
    require(jobs.cancel(71, canceledId), "translation cancellation accepted");
    translation_tests::waitUntil([&] { return provider.disconnected(2); },
                                 "translation cancellation releases provider request");
    require(jobs.get(71, canceledId)->value(QStringLiteral("status")) == QStringLiteral("canceled"),
            "translation cancellation remains distinct from provider failure");
    require(call(QStringLiteral("updates_action"),
                 {{QStringLiteral("action"), QStringLiteral("download")}})
                    .errorCode ==
#ifdef Q_OS_MACOS
                QStringLiteral("unsupported"),
#else
                QStringLiteral("invalid_state"),
#endif
            "download requires an available update");
    ScreenshotMcpRequest updateRequest;
    updateRequest.connectionId = 71;
    updateRequest.requestId = QString::number(++sequence);
    updateRequest.method = QStringLiteral("snow_shot_updates_action");
    updateRequest.params = {{QStringLiteral("action"), QStringLiteral("check")}};
    ScreenshotMcpResponse update;
    service.request(updateRequest, [&](auto response) {
        update = std::move(response);
        require(call(QStringLiteral("updates_action"),
                     {{QStringLiteral("action"), QStringLiteral("cancel")}}, {}, {}, 72)
                        .errorCode == QStringLiteral("job_not_found"),
                "another connection cannot cancel a private update job before helper startup");
    });
    require(update.ok, "update checks return owned jobs before helper startup");
    const auto updateJob = update.result.value(QStringLiteral("job_id")).toString();
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, updateJob)->value(QStringLiteral("status")) ==
                   QStringLiteral("failed");
        },
        "missing updater helper reports terminal job failure");
    require(!jobs.get(72, updateJob), "update job status remains private to its requester");
    service.disconnected(71);
    service.shutdown();
}
