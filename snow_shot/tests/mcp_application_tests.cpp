#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/storageusagetracker.h"
#include "snowimageqtcodec.h"
#include "snow_shot/storage/configurationarchive.h"
#include "../src/app/mcp/mcpsettingsadapter_p.h"
#include "translation_test_support.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/update/updateservice.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
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
template <typename Call>
void translationRetries(Call&& call, translation::TranslationService& translation,
                        McpJobRegistry& jobs, translation_tests::Server& provider,
                        int& selectionCaptures) {
    const auto catalog = call(QStringLiteral("translation_catalog"));
    require(catalog.ok, "translation catalog available");
    const auto languages = catalog.result.value(QStringLiteral("languages")).toArray();
    for (const auto& code : {QStringLiteral("hi"), QStringLiteral("th"), QStringLiteral("vi")})
        require(
            std::count_if(languages.cbegin(), languages.cend(),
                          [&](const auto& value) {
                              const auto language = value.toObject();
                              return language.value(QStringLiteral("code")) == code &&
                                     !language.value(QStringLiteral("name")).toString().isEmpty();
                          }) == 1,
            "MCP exposes each additional translation language with its name");
    translation_tests::waitUntil([&] { return !translation.loadingModels(); },
                                 "deterministic provider catalog completes");
    auto capturedPreferences = translation.preferences();
    capturedPreferences.secondaryTargetLanguage = QStringLiteral("fr");
    capturedPreferences.languageDetectionConfidenceThreshold = 60;
    require(translation.savePreferences(capturedPreferences), "set original secondary target");
    const auto started = call(QStringLiteral("translation_start"),
                              {{QStringLiteral("texts"), QJsonArray{QStringLiteral("こんにちは")}},
                               {QStringLiteral("model_id"), QStringLiteral("general")},
                               {QStringLiteral("source_language"), QStringLiteral("ja")},
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
    require(snapshotInput.has_value() &&
                snapshotInput->value(QStringLiteral("secondary_target_language")) ==
                    QStringLiteral("fr") &&
                snapshotInput->value(QStringLiteral("language_detection_confidence_threshold")) ==
                    60 &&
                !jobs.input(72, translationId),
            "retained retry input captures target and threshold and checks ownership");
    require(call(QStringLiteral("translation_start"),
                 {{QStringLiteral("retry_job_id"), translationId}}, {}, {}, 72)
                    .errorCode == QStringLiteral("job_not_found"),
            "wrong-owner retry does not disclose private job");
    auto changedPreferences = translation.preferences();
    changedPreferences.targetLanguage = QStringLiteral("de");
    changedPreferences.secondaryTargetLanguage = QStringLiteral("de");
    changedPreferences.languageDetectionConfidenceThreshold = 90;
    require(translation.savePreferences(changedPreferences), "UI changes translation preferences");
    const auto retried = call(QStringLiteral("translation_start"),
                              {{QStringLiteral("retry_job_id"), translationId}});
    require(retried.ok, "failed translation can retry");
    const auto retryId = retried.result.value(QStringLiteral("job_id")).toString();
    require(retryId != translationId && jobs.input(71, retryId) == snapshotInput,
            "retry owns a new handle and preserves captured preferences");
    translation_tests::waitUntil([&] { return provider.streams.size() == 2; },
                                 "retry reaches provider");
    require(provider.streams.at(1).body == provider.streams.at(0).body,
            "retry restores original text and both targets after shared preferences change");
    provider.delta(1, QStringLiteral("translated"));
    provider.finish(1);
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, retryId)->value(QStringLiteral("status")) ==
                   QStringLiteral("completed");
        },
        "retry completes");
    changedPreferences.secondaryTargetLanguage = QStringLiteral("pt");
    changedPreferences.languageDetectionConfidenceThreshold = 30;
    require(translation.savePreferences(changedPreferences), "change targets before chained retry");
    const auto canceled =
        call(QStringLiteral("translation_start"), {{QStringLiteral("retry_job_id"), retryId}});
    const auto canceledId = canceled.result.value(QStringLiteral("job_id")).toString();
    translation_tests::waitUntil([&] { return provider.streams.size() == 3; },
                                 "cancel fixture starts");
    require(provider.streams.at(2).body == provider.streams.at(0).body,
            "chained retry preserves the original secondary target");
    require(jobs.cancel(71, canceledId), "translation cancellation accepted");
    translation_tests::waitUntil([&] { return provider.disconnected(2); },
                                 "translation cancellation releases provider request");
    require(jobs.get(71, canceledId)->value(QStringLiteral("status")) == QStringLiteral("canceled"),
            "translation cancellation remains distinct from provider failure");
    const auto selected = call(QStringLiteral("translation_start"),
                               {{QStringLiteral("source"), QStringLiteral("selection")},
                                {QStringLiteral("model_id"), QStringLiteral("general")},
                                {QStringLiteral("source_language"), QStringLiteral("ja")},
                                {QStringLiteral("target_language"), QStringLiteral("ja")}});
    require(selected.ok, "selected text returns a translation job");
    const auto selectedId = selected.result.value(QStringLiteral("job_id")).toString();
    translation_tests::waitUntil([&] { return provider.streams.size() == 4; },
                                 "selected text reaches provider");
    provider.fail(3);
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, selectedId)->value(QStringLiteral("status")) ==
                   QStringLiteral("failed");
        },
        "selected text failure produces a retryable job");
    changedPreferences.secondaryTargetLanguage = QStringLiteral("ko");
    changedPreferences.languageDetectionConfidenceThreshold = 100;
    require(translation.savePreferences(changedPreferences), "change target after selection");
    const auto selectedRetry =
        call(QStringLiteral("translation_start"), {{QStringLiteral("retry_job_id"), selectedId}});
    require(selectedRetry.ok, "selected text can retry from retained input");
    translation_tests::waitUntil([&] { return provider.streams.size() == 5; },
                                 "selected text retry reaches provider");
    require(selectionCaptures == 1 && provider.streams.at(4).body == provider.streams.at(3).body,
            "selected text retry retains both targets without recapturing the selection");
    provider.delta(4, QStringLiteral("selected translation"));
    provider.finish(4);
    const auto selectedRetryId = selectedRetry.result.value(QStringLiteral("job_id")).toString();
    translation_tests::waitUntil(
        [&] {
            return jobs.get(71, selectedRetryId)->value(QStringLiteral("status")) ==
                   QStringLiteral("completed");
        },
        "selected text retry completes");
}
} // namespace

void runMcpApplicationReplayTests() {
    auto& storage = storage::ApplicationStorage::instance();
    presentation::GlobalShortcutManager shortcuts;
    const auto suspended = shortcuts.suspendRegistrations();
    Q_UNUSED(suspended);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession settingsSession(registry, backend);
    McpJobRegistry jobs;
    McpApplicationService::Ports ports;
    ports.storage = &storage;
    ports.settings = &settingsSession;
    ports.jobs = &jobs;
    QTemporaryDir directory;
    require(directory.isValid(), "isolated application replay output");
    ScreenshotMcpRequest original;
    original.connectionId = 71;
    original.requestId = QStringLiteral("application-original");
    original.idempotencyKey = QStringLiteral("application-export");
    original.method = QStringLiteral("snow_shot_configuration_export");
    original.params = {
        {QStringLiteral("path"), directory.filePath(QStringLiteral("settings.json"))}};
    auto service = std::make_unique<McpApplicationService>(ports);
    std::optional<ScreenshotMcpResponse> exported;
    service->request(original, [&](auto response) { exported = std::move(response); });
    require(!exported, "background export remains pending until its response is delivered");
    for (int index = 0; index < 12; ++index) {
        auto alias = original;
        alias.requestId = QStringLiteral("application-alias-%1").arg(index);
        int completions = 0;
        service->request(alias, [&](auto response) {
            require(response.errorCode == QStringLiteral("canceled") &&
                        response.requestId == alias.requestId,
                    "application alias cancellation has its own response");
            ++completions;
        });
        require(service->cancelRequest(71, alias.requestId) && completions == 1 && !exported,
                "canceling an application alias releases its slot and preserves shared work");
    }
    translation_tests::waitUntil([&] { return exported.has_value(); }, "original export completes");
    require(exported->ok && QFileInfo::exists(directory.filePath(QStringLiteral("settings.json"))),
            "shared application export succeeds after its aliases are canceled");

    original.requestId = original.idempotencyKey = QStringLiteral("application-disconnect");
    int disconnectedCompletions = 0;
    service->request(original, [&](auto) { ++disconnectedCompletions; });
    service->disconnected(71);
    service->shutdown();
    translation_tests::flushEvents();
    require(disconnectedCompletions == 0,
            "retired application requests cannot publish late replies");
    service.reset();

    service = std::make_unique<McpApplicationService>(ports);
    original.requestId = original.idempotencyKey = QStringLiteral("application-lifetime");
    int completionsAfterDestruction = 0;
    int settled = 0;
    service->request(original, [&](auto response) {
        require(response.ok, "application lifetime fixture completes");
        ++settled;
    });
    auto alias = original;
    alias.requestId = QStringLiteral("application-destroying-alias");
    service->request(alias, [&](auto) {
        ++settled;
        service.reset();
    });
    alias.requestId = QStringLiteral("application-retired-alias");
    service->request(alias, [&](auto) { ++completionsAfterDestruction; });
    translation_tests::waitUntil([&] { return !service; },
                                 "application callback destroys its service");
    require(settled == 2 && completionsAfterDestruction == 0,
            "application destruction suppresses the remaining shared callbacks");
}

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
    updateOptions.githubApiUrl = QUrl(QStringLiteral("http://127.0.0.1/releases"));
    updateOptions.giteeApiUrl = QUrl(QStringLiteral("http://127.0.0.1/releases"));
    updateOptions.storeManaged = false;
    // macOS rejects non-HTTPS before network access; Windows lacks its packaged helper here.
    update::UpdateService updates(std::move(updateOptions));
    translation_tests::Server provider;
    SnowShotApiClient api(provider.url());
    auto& translation = translation::TranslationService::forClient(
        api, configuration, QLocale::English,
        [](const QString&) -> std::optional<translation::DetectedTranslationLanguage> {
            return translation::DetectedTranslationLanguage{QStringLiteral("jpn"), 0.65};
        });
    McpApplicationService::Ports ports;
    ports.storage = &storage;
    ports.settings = &session;
    ports.jobs = &jobs;
    ports.translation = &translation;
    ports.updates = &updates;
    QByteArray historyPng;
    ports.artifactWriter = [&](quint64, QByteArray bytes, QString mime) {
        require(mime == QStringLiteral("image/png"), "history returns a PNG artifact");
        historyPng = std::move(bytes);
        return QJsonObject{{QStringLiteral("artifact_id"), QStringLiteral("history-image")}};
    };
    int selectionCaptures = 0;
    ports.selectedText = [&](auto completion) {
        ++selectionCaptures;
        completion(QStringLiteral("こんにちは"), {});
        return std::function<void()>{};
    };
    int lifecycleActions = 0;
    ports.restartAllowed = [] { return true; };
    ports.action = [&](const QString&, const QJsonObject&) {
        ++lifecycleActions;
        return true;
    };
    McpApplicationService service(ports);
#ifdef Q_OS_WIN
    {
        update::UpdateService::Options storeOptions;
        storeOptions.storeManaged = true;
        update::UpdateService storeUpdates(std::move(storeOptions));
        McpJobRegistry storeJobs;
        auto storePorts = ports;
        storePorts.updates = &storeUpdates;
        storePorts.jobs = &storeJobs;
        McpApplicationService storeService(storePorts);
        ScreenshotMcpRequest request;
        request.connectionId = 71;
        request.requestId = QStringLiteral("store-status");
        request.method = QStringLiteral("snow_shot_updates_status");
        std::optional<ScreenshotMcpResponse> response;
        storeService.request(request, [&](auto result) { response = std::move(result); });
        require(response && response->ok &&
                    response->result.value(QStringLiteral("installation")) ==
                        QStringLiteral("microsoft_store"),
                "Store update discovery identifies the Store provider");
        request.method = QStringLiteral("snow_shot_updates_action");
        for (const auto* action : {"check", "download", "apply", "cancel"}) {
            request.requestId = QStringLiteral("store-") + QString::fromLatin1(action);
            request.params = {{QStringLiteral("action"), QString::fromLatin1(action)}};
            response.reset();
            storeService.request(request, [&](auto result) { response = std::move(result); });
            require(response && response->errorCode == QStringLiteral("unsupported") &&
                        !response->result.contains(QStringLiteral("job_id")) &&
                        !storeJobs.hasRunningJobs() && !storeUpdates.busy(),
                    "Store native update actions are rejected before creating work");
        }
    }
#endif
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
    const QVector<QColor> paletteColors{QColor(0, 17, 34, 0), QColor(51, 68, 85, 128),
                                        QColor(102, 119, 136, 255), QColor(153, 170, 187, 64),
                                        QColor(204, 221, 238, 192)};
    const QJsonArray paletteJson{QStringLiteral("#00112200"), QStringLiteral("#33445580"),
                                 QStringLiteral("#667788FF"), QStringLiteral("#99AABB40"),
                                 QStringLiteral("#CCDDEEC0")};
    require(settingsJson(QVariant::fromValue(paletteColors)) == paletteJson,
            "MCP palettes encode the schema's RGBA order and preserve every alpha value");
    for (const auto binding : {settings::SettingsColorPaletteBinding::StrokeColors,
                               settings::SettingsColorPaletteBinding::FillColors}) {
        const auto* palette = registry.fieldForColorPalette(binding);
        require(palette != nullptr, "both color palette fields are registered");
        const auto publicPalette = [&](const ScreenshotMcpResponse& response) {
            for (const auto& value : response.result.value(QStringLiteral("fields")).toArray()) {
                const auto object = value.toObject();
                if (object.value(QStringLiteral("id")) == palette->id)
                    return object.value(QStringLiteral("value"));
            }
            return QJsonValue();
        };
        require(publicPalette(snapshot) == configuration.value(palette->configurationKey),
                "MCP discovery exposes the canonical persisted palette without losing alpha");
        QVariant decodedPalette;
        require(settingsValue(*palette, paletteJson, &decodedPalette) &&
                    decodedPalette.metaType() == QMetaType::fromType<QVector<QColor>>() &&
                    decodedPalette.value<QVector<QColor>>() == paletteColors,
                "MCP palette decoding produces the typed value accepted by the runtime");
        auto normalizedInput = paletteJson;
        normalizedInput[3] = QStringLiteral("  #99aabb40  ");
        require(settingsValue(*palette, normalizedInput, &decodedPalette) &&
                    decodedPalette.value<QVector<QColor>>() == paletteColors,
                "MCP palettes accept the schema's whitespace and case normalization");
        const auto paletteWrite =
            call(QStringLiteral("settings_update"),
                 {{QStringLiteral("values"), QJsonObject{{palette->id, normalizedInput}}}},
                 configuration.revision());
        require(paletteWrite.ok && session.colorPaletteValue(binding) == paletteColors &&
                    configuration.value(palette->configurationKey) == paletteJson &&
                    publicPalette(call(QStringLiteral("settings_get"))) == paletteJson,
                "MCP palette updates persist and publish the same typed RGBA values as the UI");
        auto invalidColor = paletteJson;
        invalidColor[2] = QStringLiteral("#667788");
        auto invalidElement = paletteJson;
        invalidElement[2] = 7;
        auto missingColor = paletteJson;
        missingColor.removeLast();
        auto extraColor = paletteJson;
        extraColor.append(QStringLiteral("#FFFFFFFF"));
        for (const QJsonValue& invalid :
             {QJsonValue(QStringLiteral("#00112200")), QJsonValue(invalidColor),
              QJsonValue(invalidElement), QJsonValue(missingColor), QJsonValue(extraColor)}) {
            const auto revision = configuration.revision();
            require(!settingsValue(*palette, invalid, &decodedPalette) &&
                        call(QStringLiteral("settings_update"),
                             {{QStringLiteral("values"), QJsonObject{{palette->id, invalid}}}},
                             revision)
                                .errorCode == QStringLiteral("invalid_parameters") &&
                        configuration.revision() == revision &&
                        session.colorPaletteValue(binding) == paletteColors,
                    "invalid palette shapes and colors are rejected before runtime mutation");
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

    const auto* autoHideField =
        registry.field(QStringLiteral("screen-recording.auto-hide-toolbar"));
    require(autoHideField != nullptr && autoHideField->kind == settings::SettingsFieldKind::Switch,
            "recording toolbar auto-hide is discoverable as a boolean MCP setting");
    const auto publicAutoHide = [&](const ScreenshotMcpResponse& response) {
        for (const auto& value : response.result.value(QStringLiteral("fields")).toArray()) {
            const auto object = value.toObject();
            if (object.value(QStringLiteral("id")) == autoHideField->id)
                return object;
        }
        return QJsonObject();
    };
    const auto autoHideSnapshot = publicAutoHide(snapshot);
    require(autoHideSnapshot.value(QStringLiteral("value")).isBool() &&
                autoHideSnapshot.value(QStringLiteral("page_id")) == autoHideField->pageId &&
                autoHideSnapshot.value(QStringLiteral("section_id")) == autoHideField->sectionId,
            "MCP discovery exposes toolbar auto-hide in the screen recording Interaction section");
    const auto autoHideWrite =
        call(QStringLiteral("settings_update"),
             {{QStringLiteral("values"), QJsonObject{{autoHideField->id, true}}}},
             configuration.revision());
    require(
        autoHideWrite.ok && storage::RecordingSettings().autoHideToolbar() &&
            publicAutoHide(call(QStringLiteral("settings_get"))).value(QStringLiteral("value")) ==
                true,
        "MCP toolbar auto-hide updates persist and publish the same boolean as the UI");
    const auto autoHideRevision = configuration.revision();
    require(
        call(QStringLiteral("settings_update"),
             {{QStringLiteral("values"), QJsonObject{{autoHideField->id, QStringLiteral("true")}}}},
             autoHideRevision)
                    .errorCode == QStringLiteral("invalid_parameters") &&
            configuration.revision() == autoHideRevision &&
            storage::RecordingSettings().autoHideToolbar(),
        "MCP toolbar auto-hide rejects string booleans without changing the preference");

    const auto* recordingToolbarField =
        registry.field(QStringLiteral("interface.screen-recording.recording-toolbar-editor"));
    require(recordingToolbarField != nullptr, "recording toolbar layout is discoverable over MCP");
    const auto recordingKind = storage::ScreenshotToolbarLayoutKind::RecordingActionTools;
    const auto defaultRecordingLayout = session.toolbarLayout(recordingKind);
    const auto screenshotLayout =
        session.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools);
    require(session.applySelectValue(settings::SettingsSelectBinding::ScreenRecordingFrameRate, 60),
            "prepare an unrelated recording setting before toolbar reset");
    auto recordingLayout = defaultRecordingLayout;
    recordingLayout.positions.removeIf(
        [](const QStringList& position) { return position.contains(QStringLiteral("duration")); });
    recordingLayout.hidden.append(QStringLiteral("duration"));
    std::reverse(recordingLayout.positions.begin(), recordingLayout.positions.end());
    const QJsonValue recordingJson = settingsJson(QVariant::fromValue(recordingLayout));
    QVariant decodedRecording;
    require(settingsValue(*recordingToolbarField, recordingJson, &decodedRecording) &&
                decodedRecording.value<storage::ScreenshotToolbarLayout>() == recordingLayout,
            "recording toolbar layouts have a lossless typed MCP representation");
    const auto toolbarWrite =
        call(QStringLiteral("settings_update"),
             {{QStringLiteral("values"), QJsonObject{{recordingToolbarField->id, recordingJson}}}},
             configuration.revision());
    require(toolbarWrite.ok && session.toolbarLayout(recordingKind) == recordingLayout &&
                session.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    screenshotLayout,
            "MCP recording toolbar edits preserve screenshot action layouts");

    const QJsonObject repairableRecordingJson{
        {QStringLiteral("positions"),
         QJsonArray{QJsonValue(QJsonArray{
             QStringLiteral("copy"), QStringLiteral("duration"), QStringLiteral("pause-resume"),
             QStringLiteral("separator"), QStringLiteral("start-stop"), QStringLiteral("copy"),
             QStringLiteral("unknown-action")})}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("microphone"), QStringLiteral("microphone"),
                    QStringLiteral("duration"), QStringLiteral("unknown-hidden-action")}},
    };
    const auto canonicalRecording = storage::ConfigurationSchema::normalize(
        recordingToolbarField->configurationKey, repairableRecordingJson);
    const bool recordingDecoded =
        settingsValue(*recordingToolbarField, repairableRecordingJson, &decodedRecording);
    require(canonicalRecording.valid && canonicalRecording.changed && recordingDecoded &&
                settingsJson(decodedRecording) == canonicalRecording.value,
            "MCP toolbar decoding canonicalizes singleton stacks, duplicates and omitted actions");
    const auto canonicalWrite =
        call(QStringLiteral("settings_update"),
             {{QStringLiteral("values"),
               QJsonObject{{recordingToolbarField->id, repairableRecordingJson}}}},
             configuration.revision());
    require(canonicalWrite.ok, "MCP accepts repairable recording toolbar layouts");
    if (const auto jobId = canonicalWrite.result.value(QStringLiteral("job_id")).toString();
        !jobId.isEmpty()) {
        translation_tests::waitUntil(
            [&] {
                return jobs.get(71, jobId)->value(QStringLiteral("status")) !=
                       QStringLiteral("running");
            },
            "canonical recording toolbar write completes its job");
        require(jobs.get(71, jobId)->value(QStringLiteral("status")) == QStringLiteral("completed"),
                "canonical recording toolbar job must complete without reporting a conflict");
    }
    QCoreApplication::processEvents();
    const auto recordingState = session.state(recordingToolbarField->id);
    require(settingsJson(QVariant::fromValue(session.toolbarLayout(recordingKind))) ==
                    canonicalRecording.value &&
                configuration.value(recordingToolbarField->configurationKey) ==
                    canonicalRecording.value &&
                recordingState.phase == settings::SettingsWritePhase::Clean &&
                !recordingState.busy && !recordingState.dirty && !recordingState.conflicted &&
                recordingState.error.isEmpty() && !jobs.hasRunningJobs() &&
                session.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    screenshotLayout,
            "canonical MCP toolbar writes settle cleanly and preserve unrelated action layouts");

    for (const QJsonObject& invalidRecordingJson : {
             QJsonObject{{QStringLiteral("positions"), QJsonArray{QStringLiteral("start-stop")}},
                         {QStringLiteral("hidden"), QJsonArray{}}},
             QJsonObject{{QStringLiteral("positions"), QJsonArray{QJsonValue(QJsonArray{true})}},
                         {QStringLiteral("hidden"), QJsonArray{}}},
             QJsonObject{{QStringLiteral("positions"), QJsonArray{}},
                         {QStringLiteral("hidden"), QJsonArray{1}}},
             QJsonObject{{QStringLiteral("positions"), QJsonArray{}}},
         }) {
        require(!settingsValue(*recordingToolbarField, invalidRecordingJson, &decodedRecording),
                "canonical decoding must retain strict rejection of malformed toolbar JSON types");
    }
    const auto toolbarReset =
        call(QStringLiteral("settings_reset"),
             {{QStringLiteral("page_id"), recordingToolbarField->pageId},
              {QStringLiteral("section_id"), recordingToolbarField->sectionId}},
             configuration.revision());
    require(toolbarReset.ok && session.toolbarLayout(recordingKind) == defaultRecordingLayout &&
                session.toolbarLayout(storage::ScreenshotToolbarLayoutKind::ActionTools) ==
                    screenshotLayout &&
                session.integerValue(auxiliary->binding) == delay &&
                storage::RecordingSettings().autoHideToolbar(),
            "MCP recording toolbar category reset restores only its own layout");
    require(
        session.selectValue(settings::SettingsSelectBinding::ScreenRecordingFrameRate).toInt() ==
            60,
        "recording toolbar reset preserves video preferences on the same settings page");
    const auto autoHideReset = call(QStringLiteral("settings_reset"),
                                    {{QStringLiteral("page_id"), autoHideField->pageId},
                                     {QStringLiteral("section_id"), autoHideField->sectionId}},
                                    configuration.revision());
    require(
        autoHideReset.ok && !storage::RecordingSettings().autoHideToolbar() &&
            publicAutoHide(call(QStringLiteral("settings_get"))).value(QStringLiteral("value")) ==
                false &&
            session.toolbarLayout(recordingKind) == defaultRecordingLayout &&
            session.selectValue(settings::SettingsSelectBinding::ScreenRecordingFrameRate)
                    .toInt() == 60,
        "MCP Interaction reset disables toolbar auto-hide and preserves layout and video "
        "preferences");
    if (QCoreApplication::arguments().contains(QStringLiteral("--auto-hide-settings-only"))) {
        service.shutdown();
        return;
    }
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
    model.provider = QStringLiteral("deepseek");
    require(session.applyCustomAiModels({model}), "prepare existing model");
    const auto models = call(QStringLiteral("models_list"));
    require(models.ok && !QJsonDocument(models.result).toJson().contains(model.apiKey.toUtf8()),
            "model discovery redacts secrets");
    require(models.result.value(QStringLiteral("models"))
                    .toArray()
                    .first()
                    .toObject()
                    .value(QStringLiteral("provider")) == model.provider,
            "model discovery includes the provider type");
    auto publicModel = publicModels({model}).first().toObject();
    publicModel.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
    publicModel.insert(QStringLiteral("provider"), QStringLiteral("doubao"));
    require(call(QStringLiteral("models_update"),
                 {{QStringLiteral("models"), QJsonArray{publicModel}}}, configuration.revision())
                    .ok &&
                session.customAiModels().first().apiKey == model.apiKey &&
                session.customAiModels().first().provider == QStringLiteral("doubao"),
            "omitted credential preserved for same provider endpoint");
    publicModel.insert(QStringLiteral("provider"), QStringLiteral("unknown"));
    require(!call(QStringLiteral("models_update"),
                  {{QStringLiteral("models"), QJsonArray{publicModel}}}, configuration.revision())
                    .ok &&
                session.customAiModels().first().provider == QStringLiteral("doubao"),
            "invalid provider updates preserve the previous model");
    publicModel.remove(QStringLiteral("provider"));
    require(call(QStringLiteral("models_update"),
                 {{QStringLiteral("models"), QJsonArray{publicModel}}}, configuration.revision())
                    .ok &&
                session.customAiModels().first().provider == QStringLiteral("qwen"),
            "legacy MCP model updates retain Qwen request behavior");
    publicModel.insert(QStringLiteral("provider"), QStringLiteral("doubao"));
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
    auto historyPolicy = storage.captureHistory().policy();
    historyPolicy.enabled = true;
    require(storage.captureHistory().updatePolicy(historyPolicy).get().success,
            "enable history image fixture publication");
    QImage historyImage(23, 17, QImage::Format_RGBA8888);
    historyImage.fill(QColor(21, 73, 129, 170));
    const QByteArray originalPng = image_codec::encodePng(historyImage, 0);
    storage::CaptureHistoryDraft historyDraft;
    historyDraft.contentKind = storage::CaptureHistoryContentKind::Image;
    historyDraft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    historyDraft.createdUtc = QDateTime::currentDateTimeUtc();
    historyDraft.canvasBounds = historyImage.rect();
    historyDraft.selection.rectangle = historyImage.rect();
    historyDraft.selection.shadowColor = Qt::black;
    historyDraft.canvasHistory =
        QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
    historyDraft.displays.push_back(
        {QStringLiteral("history-display"), QStringLiteral("History fixture"), historyImage});
    historyDraft.preparedResultImage =
        storage::PreparedPngImage::fromBytes(historyImage.size(), originalPng);
    const auto publishedHistory = storage.captureHistory().publish(std::move(historyDraft)).get();
    require(publishedHistory.storage.success, "publish history image fixture");
    const QJsonObject historyRequest{{QStringLiteral("history_id"), publishedHistory.record.id},
                                     {QStringLiteral("include_image"), true}};
    require(call(QStringLiteral("history_get"), historyRequest).ok && historyPng == originalPng,
            "history delivers original stored PNG bytes without recompression");
    const auto historyAssets = storage.captureHistory().displayAssets(publishedHistory.record);
    require(historyAssets && historyAssets->result, "history result file is discoverable");
    QFile damagedFile(historyAssets->result->localFileUrl.toLocalFile());
    auto damagedPng = originalPng;
    bool damaged = false;
    for (qsizetype offset = 8; offset + 12 <= damagedPng.size();) {
        const auto length = qFromBigEndian<quint32>(damagedPng.constData() + offset);
        if (length > static_cast<quint32>(damagedPng.size() - offset - 12))
            break;
        if (damagedPng.mid(offset + 4, 4) == "IDAT" && length > 0) {
            damagedPng[offset + 8] = char(damagedPng[offset + 8] ^ 0xff);
            damaged = true;
            break;
        }
        offset += qsizetype(length) + 12;
    }
    require(damaged && damagedFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                damagedFile.write(damagedPng) == damagedPng.size(),
            "damage IDAT without changing the valid PNG header or recorded byte count");
    damagedFile.close();
    require(call(QStringLiteral("history_get"), historyRequest).errorCode ==
                QStringLiteral("image_unavailable"),
            "history byte reuse still rejects corrupt PNG pixel data");
    {
        QImage largeImage(4000, 4000, QImage::Format_RGBA8888);
        largeImage.fill(QColor(21, 73, 129, 170));
        const QByteArray largePng = image_codec::encodePng(largeImage, 0);
        require(largePng.size() > 60 * 1024 * 1024,
                "low-compression history fixture exceeds the MCP response limit");
        storage::CaptureHistoryDraft largeDraft;
        largeDraft.contentKind = storage::CaptureHistoryContentKind::Image;
        largeDraft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        largeDraft.createdUtc = QDateTime::currentDateTimeUtc();
        largeDraft.canvasBounds = largeImage.rect();
        largeDraft.selection.rectangle = largeImage.rect();
        largeDraft.selection.shadowColor = Qt::black;
        largeDraft.canvasHistory =
            QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
        largeDraft.displays.push_back({QStringLiteral("large-history-display"),
                                       QStringLiteral("History fixture"), largeImage});
        largeDraft.preparedResultImage =
            storage::PreparedPngImage::fromBytes(largeImage.size(), largePng);
        const auto largeHistory = storage.captureHistory().publish(std::move(largeDraft)).get();
        require(largeHistory.storage.success, "publish large low-compression history fixture");
        require(call(QStringLiteral("history_get"),
                     {{QStringLiteral("history_id"), largeHistory.record.id},
                      {QStringLiteral("include_image"), true}})
                        .ok &&
                    historyPng.size() <= 60 * 1024 * 1024 && historyPng != largePng,
                "oversized stored PNG retains the previous default recompression fallback");
        const auto image = image_codec::decode(historyPng, snow::image::Format::png, "history.png");
        require(image.size() == largeImage.size() &&
                    image.pixelColor(0, 0) == largeImage.pixelColor(0, 0),
                "history size-limit fallback preserves dimensions and pixel content");
    }
    translationRetries(call, translation, jobs, provider, selectionCaptures);
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
        "unavailable update transport reports terminal job failure");
    require(!jobs.get(72, updateJob), "update job status remains private to its requester");
    // Responses are synchronous callback boundaries: their consumer may retire the service.
    for (int variant = 0; variant < 5; ++variant) {
        McpJobRegistry lifetimeJobs;
        auto lifetimePorts = ports;
        lifetimePorts.jobs = &lifetimeJobs;
        int captures = 0;
        lifetimePorts.selectedText = [&](auto) {
            ++captures;
            return std::function<void()>{};
        };
        auto ownedService = std::make_unique<McpApplicationService>(lifetimePorts);
        ScreenshotMcpRequest request;
        request.connectionId = 74;
        request.requestId = QString::number(++sequence);
        if (variant == 0) {
            request.method = QStringLiteral("snow_shot_updates_action");
            request.params = {{QStringLiteral("action"), QStringLiteral("check")}};
        } else if (variant == 1 || variant == 2) {
            request.method = QStringLiteral("snow_shot_translation_start");
            request.params =
                variant == 1 ? QJsonObject{{QStringLiteral("texts"),
                                            QJsonArray{QStringLiteral("response teardown")}}}
                             : QJsonObject{{QStringLiteral("source"), QStringLiteral("selection")}};
        } else if (variant == 3) {
            request.method = QStringLiteral("snow_shot_history_clear");
            request.expectedRevision = storage.captureHistory().recordsSnapshot().revision;
        } else {
            request.method = QStringLiteral("snow_shot_settings_action");
            const auto action = std::find_if(
                registry.fields().cbegin(), registry.fields().cend(), [](const auto& item) {
                    const auto* definition =
                        std::get_if<settings::SettingsActionDefinition>(&item.definition->payload);
                    return definition && definition->binding ==
                                             settings::SettingsActionBinding::ClearPinnedHistory;
                });
            require(action != registry.fields().cend(), "cleanup action is discoverable");
            request.params = {{QStringLiteral("field_id"), action->id}};
            request.expectedRevision = configuration.revision();
        }
        int responses = 0;
        ownedService->request(request, [&](auto response) {
            require(response.ok, "destructive response observer receives an accepted job");
            ++responses;
            ownedService.reset();
        });
        require(!ownedService && responses == 1 && captures == 0,
                "a response observer can retire its service before post-response work");
        translation_tests::waitUntil([&] { return !storage.status().pinnedClearing; },
                                     "cleanup finishes independently of its retired observer");
    }
    for (int variant = 0; variant < 3; ++variant) {
        McpJobRegistry lifetimeJobs;
        auto lifetimePorts = ports;
        lifetimePorts.jobs = &lifetimeJobs;
        auto ownedService = std::make_unique<McpApplicationService>(lifetimePorts);
        ScreenshotMcpRequest request;
        request.connectionId = 75;
        request.requestId = QString::number(++sequence);
        QString thumbnailPath;
        const auto removeThumbnail = qScopeGuard([&] {
            if (!thumbnailPath.isEmpty())
                QFile::remove(thumbnailPath);
        });
        if (variant == 0) {
            request.method = QStringLiteral("snow_shot_updates_action");
            request.params = {{QStringLiteral("action"), QStringLiteral("check")}};
        } else if (variant == 1) {
            request.method = QStringLiteral("snow_shot_settings_update");
            request.params = {
                {QStringLiteral("values"),
                 QJsonObject{
                     {historyField->id, !session.state(historyField->id).acceptedValue.toBool()}}}};
            request.expectedRevision = configuration.revision();
        } else {
            const QString directory =
                storage::StorageUsageTracker::defaultThumbnailCacheDirectory();
            require(!directory.isEmpty() && QDir().mkpath(directory),
                    "thumbnail cleanup fixture has a writable cache directory");
            thumbnailPath = QDir(directory).filePath(
                QStringLiteral("mcp-lifetime-%1.png")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QFile thumbnail(thumbnailPath);
            require(thumbnail.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                        thumbnail.write(QByteArray(64, 'x')) == 64,
                    "thumbnail cleanup fixture creates a removable PNG cache entry");
            thumbnail.close();
            storage.requestStorageUsageRefresh();
            require(storage.flushNow().success,
                    "thumbnail fixture usage refresh drains before cleanup admission");
            translation_tests::waitUntil(
                [&] {
                    const auto state =
                        session.actionState(settings::SettingsActionBinding::ClearThumbnailCache);
                    return state.enabled && !state.busy &&
                           storage.status().appUsage.thumbnailCacheBytes >= 64;
                },
                "thumbnail cleanup is admitted after the seeded cache usage scan finishes");
            request.method = QStringLiteral("snow_shot_settings_action");
            request.params = {
                {QStringLiteral("field_id"), QStringLiteral("storage.clear-thumbnails")}};
            request.expectedRevision = configuration.revision();
        }
        QString terminalStatus;
        QString terminalError;
        QObject::connect(&lifetimeJobs, &McpJobRegistry::changed, &session,
                         [&](quint64 owner, const QString& id) {
                             const auto state = lifetimeJobs.get(owner, id);
                             if (state && state->value(QStringLiteral("status")) !=
                                              QStringLiteral("running")) {
                                 terminalStatus = state->value(QStringLiteral("status")).toString();
                                 terminalError = state->value(QStringLiteral("result"))
                                                     .toObject()
                                                     .value(QStringLiteral("error"))
                                                     .toObject()
                                                     .value(QStringLiteral("code"))
                                                     .toString();
                                 ownedService.reset();
                             }
                         });
        int responses = 0;
        ownedService->request(request, [variant, &responses](auto response) {
            ++responses;
            const auto message =
                QStringLiteral("job observation variant %1 begins with an accepted response: %2")
                    .arg(variant)
                    .arg(response.errorCode)
                    .toUtf8();
            require(response.ok, message.constData());
        });
        translation_tests::waitUntil([&] { return !ownedService; },
                                     "terminal job observers can retire owned observations");
        require(responses == 1, "terminal observers retire their service after its accepted reply");
        if (variant == 2) {
            const auto message =
                QStringLiteral("thumbnail cleanup completes after observer retirement: "
                               "status=%1 error=%2 file_exists=%3 cache_clearing=%4")
                    .arg(terminalStatus, terminalError)
                    .arg(QFile::exists(thumbnailPath))
                    .arg(storage.status().cacheClearing)
                    .toUtf8();
            require(terminalStatus == QStringLiteral("completed") &&
                        !QFile::exists(thumbnailPath) && !storage.status().cacheClearing,
                    message.constData());
        }
    }
    for (int variant = 0; variant < 4; ++variant) {
        auto ownedService = std::make_unique<McpApplicationService>(ports);
        ScreenshotMcpRequest request;
        request.connectionId = 73;
        request.requestId = QString::number(++sequence);
        request.method = QStringLiteral("snow_shot_translation_start");
        request.params = {{QStringLiteral("texts"), QJsonArray{QStringLiteral("teardown")}},
                          {QStringLiteral("model_id"), QStringLiteral("general")}};
        std::optional<ScreenshotMcpResponse> startedJob;
        ownedService->request(request, [&](auto response) { startedJob = std::move(response); });
        require(startedJob && startedJob->ok, "service teardown owns a translation job");
        const auto id = startedJob->result.value(QStringLiteral("job_id")).toString();
        const auto child = ownedService->findChild<translation::TranslationJob*>();
        require(child != nullptr, "translation lifetime belongs to its application service");
        if (variant == 1) {
            // Retire it without dispatching DeferredDelete before its owner goes away.
            child->deleteLater();
        } else if (variant == 2) {
            require(jobs.cancel(73, id), "cancel teardown translation");
        }
        const QPointer<McpApplicationService> guard(ownedService.get());
        bool retiredWhileStopping = false;
        QObject::connect(child, &QObject::destroyed, &session, [&] {
            require(guard, "service retires child callbacks before its QObject teardown");
            guard->request(request, [&](auto response) {
                retiredWhileStopping = response.errorCode == QStringLiteral("unavailable");
            });
        });
        if (variant == 3) {
            const auto completed = QObject::connect(
                &jobs, &McpJobRegistry::changed, &session,
                [&](quint64 owner, const QString& changedId) {
                    const auto state = jobs.get(owner, changedId);
                    if (owner == 73 && changedId == id && state &&
                        state->value(QStringLiteral("status")) == QStringLiteral("completed"))
                        ownedService.reset();
                });
            const int streamCount = static_cast<int>(provider.streams.size());
            translation_tests::waitUntil([&] { return provider.streams.size() > streamCount; },
                                         "teardown translation reaches provider");
            provider.delta(streamCount, QStringLiteral("done"));
            provider.finish(streamCount);
            translation_tests::waitUntil([&] { return !ownedService; },
                                         "completion observers retire their service");
            QObject::disconnect(completed);
        }
        ownedService.reset();
        require(retiredWhileStopping,
                "active and deferred translation children retire while service state is valid");
        jobs.disconnected(73);
        translation_tests::flushEvents();
    }
    service.disconnected(71);
    service.shutdown();
}

void runMcpTranslationTests() {
    auto& storage = storage::ApplicationStorage::instance();
    presentation::GlobalShortcutManager shortcuts;
    const auto suspended = shortcuts.suspendRegistrations();
    Q_UNUSED(suspended);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    translation_tests::Server provider;
    SnowShotApiClient api(provider.url());
    auto& translation = translation::TranslationService::forClient(
        api, storage.configuration(), QLocale::English,
        [](const QString&) -> std::optional<translation::DetectedTranslationLanguage> {
            return translation::DetectedTranslationLanguage{QStringLiteral("jpn"), 0.65};
        });
    McpJobRegistry jobs;
    McpApplicationService::Ports ports;
    ports.storage = &storage;
    ports.settings = &session;
    ports.jobs = &jobs;
    ports.translation = &translation;
    int selectionCaptures = 0;
    ports.selectedText = [&](auto completion) {
        ++selectionCaptures;
        completion(QStringLiteral("こんにちは"), {});
        return std::function<void()>{};
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
        translation_tests::waitUntil([&] { return result.has_value(); },
                                     "translation request completes");
        return *result;
    };
    translationRetries(call, translation, jobs, provider, selectionCaptures);
    service.shutdown();
}
