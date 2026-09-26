#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "mcpsettingsadapter_p.h"
#include "mcpasyncwork_p.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/translation/translationlanguages.h"
#include "snow_shot/update/updateservice.h"
#include "snow_shot/platform/windows/monitorgeometry.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QScreen>
#include <QSysInfo>
#include <QThreadPool>
#include <algorithm>
#include <atomic>

namespace snow_shot::app::mcp {
namespace {
QJsonArray rectangle(const QRect& rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

ScreenshotMcpResponse reply(const ScreenshotMcpRequest& request, QJsonObject result = {}) {
    ScreenshotMcpResponse response;
    response.ok = true;
    response.requestId = request.requestId;
    response.result = std::move(result);
    if (response.result.contains(QStringLiteral("revision")))
        response.revision = response.result.value(QStringLiteral("revision")).toInteger();
    return response;
}

ScreenshotMcpResponse failure(const ScreenshotMcpRequest& request, const QString& code,
                              const QString& field = {}) {
    auto response = reply(request);
    response.ok = false;
    response.errorCode = code;
    response.errorMessage = code;
    if (!field.isEmpty())
        response.errorDetails.insert(QStringLiteral("field"), field);
    return response;
}

QJsonObject historyJson(const storage::CaptureHistoryRecord& record) {
    return {{QStringLiteral("history_id"), record.id},
            {QStringLiteral("created_utc"), record.createdUtc.toString(Qt::ISODateWithMs)},
            {QStringLiteral("canvas_bounds"), rectangle(record.canvasBounds)},
            {QStringLiteral("selection"), rectangle(record.selection.rectangle)},
            {QStringLiteral("bytes"), record.totalBytes},
            {QStringLiteral("source"), static_cast<int>(record.source)},
            {QStringLiteral("scrolling"), record.scrolling.value_or(false)},
            {QStringLiteral("has_image"), record.result.has_value()}};
}

QJsonObject storageJson(const storage::StorageStatus& state) {
    return {{QStringLiteral("read_available"), state.readAvailable},
            {QStringLiteral("write_available"), state.writeAvailable},
            {QStringLiteral("directory"), state.effectiveDirectory},
            {QStringLiteral("history_entries"), state.historyUsage.entryCount},
            {QStringLiteral("history_bytes"), state.historyUsage.totalBytes},
            {QStringLiteral("pinned_bytes"), state.appUsage.pinnedWindowBytes},
            {QStringLiteral("thumbnail_cache_bytes"), state.appUsage.thumbnailCacheBytes},
            {QStringLiteral("recording_temp_bytes"), state.appUsage.recordingTempBytes},
            {QStringLiteral("total_bytes"), state.appUsage.totalBytes()},
            {QStringLiteral("scanning"), state.appUsage.scanning},
            {QStringLiteral("maintenance_busy"),
             state.historyClearing || state.pinnedClearing || state.cacheClearing},
            {QStringLiteral("diagnostics_available"), state.diagnostics.loggingAvailable},
            {QStringLiteral("diagnostics_dropped_records"),
             static_cast<qint64>(state.diagnostics.droppedRecords)}};
}

QJsonObject permissionJson(presentation::AppPermissionService* service) {
    QJsonArray permissions;
    if (service) {
        static const QStringList names{QStringLiteral("checking"), QStringLiteral("granted"),
                                       QStringLiteral("missing"),  QStringLiteral("not_determined"),
                                       QStringLiteral("denied"),   QStringLiteral("restricted"),
                                       QStringLiteral("error")};
        for (int index = 0; index < 4; ++index) {
            const auto permission = static_cast<presentation::AppPermission>(index);
            permissions.append(
                QJsonObject{{QStringLiteral("id"), presentation::appPermissionId(permission)},
                            {QStringLiteral("status"),
                             names.value(static_cast<int>(service->snapshot().status(permission)))},
                            {QStringLiteral("pending"), service->requestPending(permission)}});
        }
    }
    return {{QStringLiteral("permissions"), permissions}};
}

QJsonObject updateJson(update::UpdateService* service) {
    if (!service)
        return {{QStringLiteral("state"), QStringLiteral("unavailable")}};
    const auto& status = service->status();
    static const QStringList states{
        QStringLiteral("unavailable"), QStringLiteral("idle"),        QStringLiteral("checking"),
        QStringLiteral("available"),   QStringLiteral("downloading"), QStringLiteral("verifying"),
        QStringLiteral("ready"),       QStringLiteral("applying"),    QStringLiteral("failed")};
    return {{QStringLiteral("state"), states.value(static_cast<int>(status.state))},
            {QStringLiteral("version"), status.version},
            {QStringLiteral("received_bytes"), status.received},
            {QStringLiteral("total_bytes"), status.total},
#ifdef Q_OS_MACOS
            {QStringLiteral("installation"), QStringLiteral("official_website")}
#else
            {QStringLiteral("installation"), QStringLiteral("native_updater")}
#endif
    };
}
} // namespace

struct McpApplicationService::Impl {
    McpApplicationService& q;
    Ports ports;
    QThreadPool workers;
    bool stopping = false;
    QHash<QString, QPointer<translation::TranslationJob>> translations;
    QString activeUpdateJob;
    quint64 activeUpdateOwner = 0;
    int admittedWork = 0;
    QHash<QString, std::shared_ptr<std::atomic_bool>> pendingReads;
    struct Replay {
        QByteArray fingerprint;
        ScreenshotMcpResponse response;
        QList<ScreenshotMcpServer::Completion> waiters;
        quint64 owner = 0;
        qint64 created = 0;
        bool complete = false;
    };
    QHash<QString, Replay> replay;
    QElapsedTimer clock;

    Impl(McpApplicationService& owner, Ports value) : q(owner), ports(std::move(value)) {
        workers.setMaxThreadCount(2);
        workers.setExpiryTimeout(30000);
        clock.start();
    }

    storage::ConfigurationStore& configuration() const {
        return ports.storage->configuration();
    }

    QJsonObject settingsSnapshot(const QString& section) const {
        QJsonArray fields;
        auto& session = *ports.settings;
        for (const auto& field : session.registry().fields()) {
            if (!section.isEmpty() && field.sectionId != section && field.pageId != section)
                continue;
            const auto state = session.state(field.id);
            QJsonObject item{{QStringLiteral("id"), field.id},
                             {QStringLiteral("page_id"), field.pageId},
                             {QStringLiteral("section_id"), field.sectionId},
                             {QStringLiteral("kind"), static_cast<int>(field.kind)},
                             {QStringLiteral("value"), settingsJson(state.acceptedValue)},
                             {QStringLiteral("enabled"), state.enabled},
                             {QStringLiteral("pending"), state.busy},
                             {QStringLiteral("failed"),
                              state.phase == settings::SettingsWritePhase::Failed ||
                                  state.phase == settings::SettingsWritePhase::Rejected},
                             {QStringLiteral("title"), field.definition->title.translated()}};
            if (const auto* schema = storage::ConfigurationSchema::entry(field.configurationKey)) {
                if (schema->integerRange)
                    item.insert(
                        QStringLiteral("range"),
                        QJsonObject{{QStringLiteral("minimum"), schema->integerRange->minimum},
                                    {QStringLiteral("maximum"), schema->integerRange->maximum},
                                    {QStringLiteral("step"), schema->integerRange->step}});
                if (!schema->allowedStringValues.isEmpty())
                    item.insert(QStringLiteral("allowed_values"),
                                QJsonArray::fromStringList(schema->allowedStringValues));
            }
            if (const auto* select =
                    std::get_if<settings::SettingsSelectDefinition>(&field.definition->payload)) {
                QJsonArray options;
                for (const auto& option : session.dynamicSelectOptions(select->binding))
                    options.append(
                        QJsonObject{{QStringLiteral("value"), settingsJson(option.value)},
                                    {QStringLiteral("label"), option.label}});
                item.insert(QStringLiteral("options"), options);
            }
            fields.append(item);
            if (const auto auxiliary = auxiliaryIntegerSetting(field)) {
                const int value = session.integerValue(auxiliary->binding);
                item.insert(QStringLiteral("id"), auxiliary->id);
                item.insert(QStringLiteral("parent_id"), field.id);
                item.insert(QStringLiteral("kind"),
                            static_cast<int>(settings::SettingsFieldKind::Integer));
                item.insert(QStringLiteral("value"), value);
                item.insert(QStringLiteral("title"),
                            field.definition->title.translated().arg(value));
                item.remove(QStringLiteral("options"));
                const auto* schema =
                    storage::ConfigurationSchema::entry(auxiliary->configurationKey);
                if (schema && schema->integerRange)
                    item.insert(
                        QStringLiteral("range"),
                        QJsonObject{{QStringLiteral("minimum"), schema->integerRange->minimum},
                                    {QStringLiteral("maximum"), schema->integerRange->maximum},
                                    {QStringLiteral("step"), schema->integerRange->step}});
                fields.append(item);
            }
        }
        return {{QStringLiteral("revision"), static_cast<qint64>(configuration().revision())},
                {QStringLiteral("fields"), fields}};
    }

    void observeSettings(const ScreenshotMcpRequest& request, const QString& settingsJob,
                         const QMap<QString, quint64>& pending, const QStringList& applied,
                         ScreenshotMcpResponse& response) {
        if (pending.isEmpty()) {
            ports.jobs->discard(request.connectionId, settingsJob);
        } else {
            auto& details = response.ok ? response.result : response.errorDetails;
            details.insert(QStringLiteral("job_id"), settingsJob);
            details.insert(QStringLiteral("pending_fields"),
                           QJsonArray::fromStringList(pending.keys()));
            auto* observation = new QObject(&q);
            const auto observe = [this, observation, settingsJob, owner = request.connectionId,
                                  pending, applied] {
                const auto job = ports.jobs->get(owner, settingsJob);
                if (!job || job->value(QStringLiteral("status")) != QStringLiteral("running")) {
                    observation->deleteLater();
                    return;
                }
                auto completed = applied;
                for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
                    const auto state = ports.settings->state(it.key());
                    if (state.revision != it.value() || state.conflicted ||
                        state.phase == settings::SettingsWritePhase::Failed ||
                        state.phase == settings::SettingsWritePhase::Rejected) {
                        ports.jobs->fail(settingsJob, QStringLiteral("settings_failed"));
                        observation->deleteLater();
                        return;
                    }
                    if (state.busy || state.phase == settings::SettingsWritePhase::Pending)
                        return;
                    completed.append(it.key());
                }
                ports.jobs->complete(
                    settingsJob,
                    {{QStringLiteral("revision"), static_cast<qint64>(configuration().revision())},
                     {QStringLiteral("applied_fields"), QJsonArray::fromStringList(completed)}});
                observation->deleteLater();
            };
            QObject::connect(ports.settings, &settings::SettingsRuntimeSession::fieldChanged,
                             observation, [observe](const auto&, const auto&) { observe(); });
            QObject::connect(ports.jobs, &McpJobRegistry::changed, observation,
                             [this, observation, owner = request.connectionId,
                              settingsJob](quint64 changedOwner, const QString& changedId) {
                                 if (changedOwner == owner && changedId == settingsJob &&
                                     !ports.jobs->get(owner, settingsJob))
                                     observation->deleteLater();
                             });
            observe();
        }
    }

    // Work admitted here only uses immutable input and thread-safe storage APIs.
    void background(const ScreenshotMcpRequest& request,
                    std::function<ScreenshotMcpResponse()> work,
                    ScreenshotMcpServer::Completion completion, bool cancellable = true) {
        if (admittedWork >= 2) {
            completion(failure(request, QStringLiteral("queue_full")));
            return;
        }
        ++admittedWork;
        const QString key = QString::number(request.connectionId) + u':' + request.requestId;
        auto canceled = std::make_shared<std::atomic_bool>(false);
        if (cancellable)
            pendingReads.insert(key, canceled);
        auto* watcher = new QFutureWatcher<ScreenshotMcpResponse>(&q);
        QObject::connect(
            watcher, &QFutureWatcher<ScreenshotMcpResponse>::finished, &q,
            [this, watcher, request, key, canceled, completion = std::move(completion)]() mutable {
                auto response = watcher->result();
                watcher->deleteLater();
                --admittedWork;
                pendingReads.remove(key);
                if (stopping || canceled->load())
                    response = failure(request, QStringLiteral("canceled"));
                completion(std::move(response));
            });
        watcher->setFuture(runMcpWork(&workers, [request, canceled, work = std::move(work)] {
            if (canceled->load())
                return failure(request, QStringLiteral("canceled"));
            try {
                return work();
            } catch (...) {
                return failure(request, QStringLiteral("operation_failed"));
            }
        }));
    }

    void handle(const ScreenshotMcpRequest&, ScreenshotMcpServer::Completion);

    void translate(const QString& id, const QStringList& texts,
                   const translation::TranslationPreferences& preferences) {
        if (!ports.jobs->retainInput(
                id, {{QStringLiteral("texts"), QJsonArray::fromStringList(texts)},
                     {QStringLiteral("source_language"), preferences.sourceLanguage},
                     {QStringLiteral("target_language"), preferences.targetLanguage},
                     {QStringLiteral("model_id"), preferences.modelId}})) {
            ports.jobs->fail(id, QStringLiteral("capacity_exceeded"));
            return;
        }
        auto* job = ports.translation->createJob(texts, preferences, &q);
        if (!ports.jobs->setCancellation(id, [guard = QPointer<translation::TranslationJob>(job)] {
                if (guard) {
                    guard->cancel();
                    guard->deleteLater();
                }
            })) {
            delete job;
            return;
        }
        translations.insert(id, job);
        QObject::connect(job, &QObject::destroyed, &q, [this, id] { translations.remove(id); });
        auto snapshot = [job] {
            QJsonArray units;
            for (const auto& unit : job->units())
                units.append(QJsonObject{{QStringLiteral("source"), unit.sourceText},
                                         {QStringLiteral("text"), unit.text},
                                         {QStringLiteral("state"), static_cast<int>(unit.state)}});
            return QJsonObject{{QStringLiteral("units"), units}};
        };
        QObject::connect(
            job, &translation::TranslationJob::unitChanged, &q, [this, id, job](int index) {
                const auto units = job->units();
                if (index < 0 || index >= units.size())
                    return;
                const auto& unit = units.at(index);
                ports.jobs->progress(id, {{QStringLiteral("unit"), index},
                                          {QStringLiteral("unit_count"), units.size()},
                                          {QStringLiteral("state"), static_cast<int>(unit.state)},
                                          {QStringLiteral("text"), unit.text.left(2048)}});
            });
        auto done = [this, id, job, snapshot] {
            if (!translations.remove(id))
                return;
            if (job->state() == translation::TranslationJob::State::Completed)
                ports.jobs->complete(id, snapshot());
            else
                ports.jobs->fail(id, QStringLiteral("translation_failed"));
            job->deleteLater();
        };
        QObject::connect(job, &translation::TranslationJob::finished, &q, done);
        QObject::connect(job, &translation::TranslationJob::invalidated, &q, done);
        job->start();
    }

    void storageJob(const ScreenshotMcpRequest& request,
                    const std::function<std::shared_future<storage::StorageResult>()>& start,
                    ScreenshotMcpServer::Completion completion) {
        if (admittedWork >= 2) {
            completion(failure(request, QStringLiteral("queue_full")));
            return;
        }
        const auto id = ports.jobs->start(request.connectionId, request.method);
        if (id.isEmpty()) {
            completion(failure(request, QStringLiteral("queue_full")));
            return;
        }
        const auto future = start();
        completion(reply(request, {{QStringLiteral("job_id"), id}}));
        background(
            request,
            [future, request] {
                const auto result = future.get();
                return result.success ? reply(request, {{QStringLiteral("completed"), true}})
                                      : failure(request, result.error == u"stale_revision"
                                                             ? QStringLiteral("stale_revision")
                                                             : QStringLiteral("storage_failed"));
            },
            [this, id](ScreenshotMcpResponse result) {
                if (result.ok)
                    ports.jobs->complete(id, result.result);
                else
                    ports.jobs->fail(id, result.errorCode);
            },
            false);
    }
};

McpApplicationService::McpApplicationService(Ports ports, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(ports))) {}
McpApplicationService::~McpApplicationService() {
    shutdown();
}

QStringList McpApplicationService::methods() {
    return {QStringLiteral("snow_shot_app_status"),
            QStringLiteral("snow_shot_app_displays"),
            QStringLiteral("snow_shot_app_action"),
            QStringLiteral("snow_shot_settings_get"),
            QStringLiteral("snow_shot_settings_update"),
            QStringLiteral("snow_shot_settings_reset"),
            QStringLiteral("snow_shot_settings_action"),
            QStringLiteral("snow_shot_models_list"),
            QStringLiteral("snow_shot_models_update"),
            QStringLiteral("snow_shot_credentials_set"),
            QStringLiteral("snow_shot_history_list"),
            QStringLiteral("snow_shot_history_get"),
            QStringLiteral("snow_shot_history_delete"),
            QStringLiteral("snow_shot_history_clear"),
            QStringLiteral("snow_shot_history_action"),
            QStringLiteral("snow_shot_configuration_export"),
            QStringLiteral("snow_shot_configuration_import"),
            QStringLiteral("snow_shot_storage_status"),
            QStringLiteral("snow_shot_storage_cleanup"),
            QStringLiteral("snow_shot_permissions_get"),
            QStringLiteral("snow_shot_permissions_request"),
            QStringLiteral("snow_shot_updates_status"),
            QStringLiteral("snow_shot_updates_action"),
            QStringLiteral("snow_shot_templates_list"),
            QStringLiteral("snow_shot_templates_update"),
            QStringLiteral("snow_shot_translation_catalog"),
            QStringLiteral("snow_shot_translation_start")};
}
bool McpApplicationService::handles(const QString& method) const {
    return methods().contains(method);
}
void McpApplicationService::request(const ScreenshotMcpRequest& request,
                                    ScreenshotMcpServer::Completion completion) {
    if (m_impl->stopping || !m_impl->ports.storage || !m_impl->ports.settings) {
        completion(failure(request, QStringLiteral("unavailable")));
        return;
    }
    if (request.idempotencyKey.isEmpty()) {
        m_impl->handle(request, std::move(completion));
        return;
    }
    // A connection owns its replay records. Never replay commands on a new connection.
    const QString key = QString::number(request.connectionId) + u':' + request.idempotencyKey;
    const QByteArray fingerprint = QCryptographicHash::hash(
        QJsonDocument(QJsonObject{{QStringLiteral("method"), request.method},
                                  {QStringLiteral("params"), request.params},
                                  {QStringLiteral("revision"),
                                   request.expectedRevision
                                       ? QJsonValue(static_cast<qint64>(*request.expectedRevision))
                                       : QJsonValue()}})
            .toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
    for (auto it = m_impl->replay.begin(); it != m_impl->replay.end();) {
        if (it->complete && m_impl->clock.elapsed() - it->created > 600000)
            it = m_impl->replay.erase(it);
        else
            ++it;
    }
    if (auto it = m_impl->replay.find(key); it != m_impl->replay.end()) {
        if (it->fingerprint != fingerprint) {
            completion(failure(request, QStringLiteral("idempotency_conflict")));
        } else if (it->complete) {
            auto result = it->response;
            result.requestId = request.requestId;
            completion(std::move(result));
        } else if (it->waiters.size() < 8) {
            it->waiters.append(
                [request, completion = std::move(completion)](auto response) mutable {
                    response.requestId = request.requestId;
                    response.afterSend = {};
                    completion(std::move(response));
                });
        } else
            completion(failure(request, QStringLiteral("queue_full")));
        return;
    }
    if (m_impl->replay.size() >= 128) {
        completion(failure(request, QStringLiteral("queue_full")));
        return;
    }
    Impl::Replay record;
    record.fingerprint = fingerprint;
    record.owner = request.connectionId;
    record.created = m_impl->clock.elapsed();
    m_impl->replay.insert(key, std::move(record));
    m_impl->handle(request, [this, key, completion = std::move(completion)](auto response) mutable {
        auto it = m_impl->replay.find(key);
        QList<ScreenshotMcpServer::Completion> waiters;
        if (it != m_impl->replay.end()) {
            waiters = std::move(it->waiters);
            // Reads with binary results are not retained a second time.
            if (!response.attachment.isEmpty() ||
                QJsonDocument(response.result).toJson(QJsonDocument::Compact).size() > 65536) {
                m_impl->replay.erase(it);
            } else {
                it->response = response;
                it->response.afterSend = {};
                it->complete = true;
            }
        }
        for (auto& waiter : waiters)
            waiter(response);
        completion(std::move(response));
    });
}
void McpApplicationService::disconnected(quint64 connection) {
    const auto prefix = QString::number(connection) + u':';
    for (auto it = m_impl->pendingReads.begin(); it != m_impl->pendingReads.end(); ++it)
        if (it.key().startsWith(prefix))
            it.value()->store(true);
    for (auto it = m_impl->replay.begin(); it != m_impl->replay.end();)
        if (it->owner == connection)
            it = m_impl->replay.erase(it);
        else
            ++it;
}
bool McpApplicationService::cancelRequest(quint64 connection, const QString& requestId) {
    const auto canceled =
        m_impl->pendingReads.value(QString::number(connection) + u':' + requestId);
    if (!canceled)
        return false;
    canceled->store(true);
    return true;
}
void McpApplicationService::shutdown() {
    if (!m_impl || m_impl->stopping)
        return;
    m_impl->stopping = true;
    m_impl->workers.waitForDone();
}

void McpApplicationService::Impl::handle(const ScreenshotMcpRequest& request,
                                         ScreenshotMcpServer::Completion completion) {
    const auto& method = request.method;
    const auto& params = request.params;
    const auto expected = request.expectedRevision;
    auto finish = [&](QJsonObject result) { completion(reply(request, std::move(result))); };
    auto error = [&](const QString& code, const QString& field = QString()) {
        completion(failure(request, code, field));
    };
    auto change = [&](const std::function<bool()>& mutation) {
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        bool conflict = false;
        const bool ok = configuration().mutateIfRevision(*expected, mutation, &conflict);
        if (!ok) {
            auto response = failure(request, conflict ? QStringLiteral("stale_revision")
                                                      : QStringLiteral("operation_failed"));
            response.revision = configuration().revision();
            completion(std::move(response));
            return;
        }
        finish({{QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
    };

    if (method == u"snow_shot_app_status") {
        finish(
            {{QStringLiteral("version"), QCoreApplication::applicationVersion()},
             {QStringLiteral("platform"), QSysInfo::productType()},
             {QStringLiteral("storage"), storageJson(ports.storage->status())},
             {QStringLiteral("settings_revision"), static_cast<qint64>(configuration().revision())},
             {QStringLiteral("permissions"), permissionJson(ports.permissions)},
             {QStringLiteral("updates"), updateJson(ports.updates)}});
    } else if (method == u"snow_shot_app_displays") {
        QJsonArray displays;
        for (auto* screen : QGuiApplication::screens()) {
            auto display = QJsonObject{
                {QStringLiteral("id"), screen->name()},
                {QStringLiteral("name"), screen->name()},
                {QStringLiteral("logical_bounds"), rectangle(screen->geometry())},
                {QStringLiteral("scale"), screen->devicePixelRatio()},
                {QStringLiteral("primary"), screen == QGuiApplication::primaryScreen()}};
            const auto physical = platform::windows::nativeMonitorRect(*screen);
            if (!physical.isEmpty())
                display.insert(QStringLiteral("physical_bounds"), rectangle(physical));
            displays.append(display);
        }
        finish({{QStringLiteral("displays"), displays}});
    } else if (method == u"snow_shot_app_action") {
        const auto action = params.value(QStringLiteral("action")).toString();
        if (action == u"restart" || action == u"quit") {
            if (!ports.restartAllowed || !ports.restartAllowed()) {
                error(QStringLiteral("busy"));
                return;
            }
            auto response = reply(request, {{QStringLiteral("accepted"), true}});
            response.afterSend = [guard = QPointer<McpApplicationService>(&q), action, params] {
                if (guard && guard->m_impl->ports.action)
                    guard->m_impl->ports.action(action, params);
            };
            completion(std::move(response));
        } else if (ports.action && ports.action(action, params)) {
            finish({{QStringLiteral("accepted"), true}});
        } else
            error(QStringLiteral("invalid_parameters"), QStringLiteral("action"));
    } else if (method == u"snow_shot_settings_get") {
        finish(settingsSnapshot(params.value(QStringLiteral("section")).toString()));
    } else if (method == u"snow_shot_settings_update") {
        const auto values = params.value(QStringLiteral("values")).toObject();
        QMap<QString, QVariant> drafts;
        QHash<QString, AuxiliaryIntegerSetting> auxiliaries;
        for (const auto& field : ports.settings->registry().fields())
            if (const auto auxiliary = auxiliaryIntegerSetting(field))
                auxiliaries.insert(auxiliary->id, *auxiliary);
        for (auto it = values.begin(); it != values.end(); ++it) {
            const auto* field = ports.settings->registry().field(it.key());
            QVariant value;
            if (const auto auxiliary = auxiliaries.constFind(it.key());
                auxiliary != auxiliaries.cend()) {
                const auto json = it.value();
                if (!json.isDouble() || !std::isfinite(json.toDouble()) ||
                    std::floor(json.toDouble()) != json.toDouble() ||
                    !storage::ConfigurationSchema::normalize(auxiliary->configurationKey, json)
                         .valid) {
                    error(QStringLiteral("invalid_parameters"), it.key());
                    return;
                }
                value = json.toInt();
            } else if (!field || !settingsValue(*field, it.value(), &value)) {
                error(QStringLiteral("invalid_parameters"), it.key());
                return;
            }
            drafts.insert(it.key(), value);
        }
        if (drafts.isEmpty()) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("values"));
            return;
        }
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        // Reserve observation capacity before accepting any runtime mutation. Synchronous
        // patches release the unpublished slot; asynchronous commits retain an owned job.
        const auto settingsJob = ports.jobs->start(request.connectionId, method);
        if (settingsJob.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        QStringList applied;
        QMap<QString, quint64> pending;
        QString rejected;
        bool conflict = false;
        const bool accepted = configuration().mutateIfRevision(
            *expected,
            [&] {
                for (auto it = drafts.cbegin(); it != drafts.cend(); ++it) {
                    const auto auxiliary = auxiliaries.constFind(it.key());
                    const bool submitted = auxiliary == auxiliaries.cend()
                                               ? ports.settings->submitDraft(it.key(), it.value())
                                               : ports.settings->applyIntegerValue(
                                                     auxiliary->binding, it.value().toInt());
                    if (!submitted) {
                        rejected = it.key();
                        return false;
                    }
                    const auto state = ports.settings->state(it.key());
                    if (auxiliary == auxiliaries.cend() &&
                        (state.phase == settings::SettingsWritePhase::Failed ||
                         state.phase == settings::SettingsWritePhase::Rejected)) {
                        rejected = it.key();
                        return false;
                    }
                    if (auxiliary == auxiliaries.cend() &&
                        (state.busy || state.phase == settings::SettingsWritePhase::Pending))
                        pending.insert(it.key(), state.revision);
                    else
                        applied.append(it.key());
                }
                return true;
            },
            &conflict);
        auto response =
            accepted
                ? reply(request,
                        {{QStringLiteral("revision"),
                          static_cast<qint64>(configuration().revision())},
                         {QStringLiteral("applied_fields"), QJsonArray::fromStringList(applied)}})
                : failure(request,
                          conflict ? QStringLiteral("stale_revision")
                                   : QStringLiteral("operation_failed"),
                          rejected);
        response.revision = configuration().revision();
        if (!accepted && !applied.isEmpty())
            response.errorDetails.insert(QStringLiteral("applied_fields"),
                                         QJsonArray::fromStringList(applied));
        observeSettings(request, settingsJob, pending, applied, response);
        completion(std::move(response));
    } else if (method == u"snow_shot_settings_reset") {
        const auto* section = ports.settings->registry().catalog().section(
            params.value(QStringLiteral("page_id")).toString(),
            params.value(QStringLiteral("section_id")).toString());
        if (!section || section->reset == settings::SettingsSectionReset::None) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("section_id"));
            return;
        }
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        const auto job = ports.jobs->start(request.connectionId, method);
        if (job.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        QMap<QString, quint64> pending;
        QStringList applied;
        bool conflict = false;
        const bool accepted = configuration().mutateIfRevision(
            *expected, [&] { return ports.settings->reset(section->reset); }, &conflict);
        if (!conflict) {
            for (const auto index : ports.settings->registry().fieldsForReset(section->reset)) {
                const auto& field = ports.settings->registry().fields().at(index);
                const auto state = ports.settings->state(field.id);
                if (state.busy || state.phase == settings::SettingsWritePhase::Pending)
                    pending.insert(field.id, state.revision);
                else if (state.phase == settings::SettingsWritePhase::Clean)
                    applied.append(field.id);
            }
        }
        auto response = accepted ? reply(request, {{QStringLiteral("applied_fields"),
                                                    QJsonArray::fromStringList(applied)}})
                                 : failure(request, conflict ? QStringLiteral("stale_revision")
                                                             : QStringLiteral("operation_failed"));
        response.revision = configuration().revision();
        observeSettings(request, job, pending, applied, response);
        completion(std::move(response));
    } else if (method == u"snow_shot_models_list") {
        finish({{QStringLiteral("models"), publicModels(ports.settings->customAiModels())},
                {QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
    } else if (method == u"snow_shot_models_update" || method == u"snow_shot_credentials_set") {
        auto models = ports.settings->customAiModels();
        if (method == u"snow_shot_credentials_set") {
            const auto id = params.value(QStringLiteral("provider")).toString();
            auto found = std::find_if(models.begin(), models.end(),
                                      [&](const auto& item) { return item.id == id; });
            if (found == models.end()) {
                error(QStringLiteral("not_found"));
                return;
            }
            found->apiKey = params.value(QStringLiteral("secret")).toString();
        } else {
            auto input = params.value(QStringLiteral("models")).toArray();
            for (qsizetype i = 0; i < input.size(); ++i) {
                auto object = input[i].toObject();
                if (!object.contains(QStringLiteral("api_key"))) {
                    QString secret;
                    for (const auto& model : models)
                        if (model.id == object.value(QStringLiteral("id")).toString() &&
                            model.baseUrl == object.value(QStringLiteral("base_url")).toString())
                            secret = model.apiKey;
                    object.insert(QStringLiteral("api_key"), secret);
                }
                if (!object.contains(QStringLiteral("supports_vision")))
                    object.insert(QStringLiteral("supports_vision"), false);
                input[i] = object;
            }
            bool valid = false;
            models = customAiModelsFromJson(input, &valid);
            if (!valid) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("models"));
                return;
            }
        }
        change([&] { return ports.settings->applyCustomAiModels(models); });
    } else if (method == u"snow_shot_storage_status") {
        auto result = storageJson(ports.storage->status());
        result.insert(QStringLiteral("revision"), static_cast<qint64>(configuration().revision()));
        finish(result);
    } else if (method == u"snow_shot_permissions_get") {
        finish(permissionJson(ports.permissions));
    } else if (method == u"snow_shot_permissions_request") {
        if (!ports.permissions) {
            error(QStringLiteral("unsupported"));
            return;
        }
        for (int i = 0; i < 4; ++i) {
            const auto permission = static_cast<presentation::AppPermission>(i);
            if (presentation::appPermissionId(permission) !=
                params.value(QStringLiteral("permission")).toString())
                continue;
            if (params.value(QStringLiteral("open_settings")).toBool()) {
                if (!ports.permissions->openSettings(permission)) {
                    error(QStringLiteral("unsupported"));
                    return;
                }
            } else
                ports.permissions->request(permission);
            finish(permissionJson(ports.permissions));
            return;
        }
        error(QStringLiteral("invalid_parameters"), QStringLiteral("permission"));
    } else if (method == u"snow_shot_updates_status") {
        finish(updateJson(ports.updates));
    } else if (method == u"snow_shot_updates_action") {
        if (!ports.updates) {
            error(QStringLiteral("unsupported"));
            return;
        }
        const auto action = params.value(QStringLiteral("action")).toString();
        if (action == u"check" || action == u"download") {
#ifdef Q_OS_MACOS
            if (action == u"download") {
                error(QStringLiteral("unsupported"));
                return;
            }
#endif
            if (ports.updates->busy() || !activeUpdateJob.isEmpty()) {
                error(QStringLiteral("busy"));
                return;
            }
            if (action == u"download" &&
                ports.updates->status().state != update::UpdateState::Available) {
                error(QStringLiteral("invalid_state"));
                return;
            }
            const auto id = ports.jobs->start(request.connectionId, method);
            if (id.isEmpty()) {
                error(QStringLiteral("queue_full"));
                return;
            }
            activeUpdateJob = id;
            activeUpdateOwner = request.connectionId;
            auto* observation = new QObject(&q);
            ports.jobs->setCancellation(id, [guard = QPointer<McpApplicationService>(&q),
                                             observation = QPointer<QObject>(observation), id] {
                if (guard && guard->m_impl->activeUpdateJob == id) {
                    guard->m_impl->activeUpdateJob.clear();
                    guard->m_impl->ports.updates->cancel();
                }
                if (observation)
                    observation->deleteLater();
            });
            QObject::connect(ports.updates, &update::UpdateService::statusChanged, observation,
                             [this, id] {
                                 if (activeUpdateJob != id)
                                     return;
                                 ports.jobs->progress(id, updateJson(ports.updates));
                             });
            QObject::connect(
                ports.updates, &update::UpdateService::operationFinished, observation,
                [this, observation, id, action](const QString& operation, const QString& outcome) {
                    if (activeUpdateJob != id || operation != action)
                        return;
                    activeUpdateJob.clear();
                    if (outcome == u"success")
                        ports.jobs->complete(id, updateJson(ports.updates));
                    else if (outcome == u"cancelled")
                        ports.jobs->cancel(activeUpdateOwner, id);
                    else
                        ports.jobs->fail(id, QStringLiteral("update_failed"));
                    observation->deleteLater();
                });
            finish({{QStringLiteral("job_id"), id}});
            if (action == u"check")
                ports.updates->check();
            else
                ports.updates->download();
            return;
        } else if (action == u"cancel") {
            if (!activeUpdateJob.isEmpty() && activeUpdateOwner != request.connectionId) {
                error(QStringLiteral("job_not_found"));
                return;
            }
            if (!activeUpdateJob.isEmpty())
                ports.jobs->cancel(activeUpdateOwner, activeUpdateJob);
            else
                ports.updates->cancel();
        } else if (action == u"apply") {
            if (ports.updates->status().state != update::UpdateState::Ready) {
                error(QStringLiteral("invalid_state"));
                return;
            }
            if (!ports.restartAllowed || !ports.restartAllowed()) {
                error(QStringLiteral("busy"));
                return;
            }
            auto response = reply(request, {{QStringLiteral("accepted"), true}});
            response.afterSend = [service = QPointer<update::UpdateService>(ports.updates)] {
                if (service)
                    service->beginApply();
            };
            completion(std::move(response));
            return;
        } else {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("action"));
            return;
        }
        finish(updateJson(ports.updates));
    } else if (method == u"snow_shot_history_list") {
        const auto snapshot = ports.storage->captureHistory().recordsSnapshot();
        const auto cursor = params.value(QStringLiteral("cursor")).toString();
        qsizetype offset = 0;
        if (!cursor.isEmpty()) {
            const auto parts = cursor.split(u':');
            bool revisionOk = false, offsetOk = false;
            const auto revision = parts.value(0).toULongLong(&revisionOk);
            offset = parts.value(1).toLongLong(&offsetOk);
            if (parts.size() != 2 || !revisionOk || !offsetOk || revision != snapshot.revision ||
                offset < 0 || offset > snapshot.records.size()) {
                error(QStringLiteral("invalid_cursor"));
                return;
            }
        }
        const int limit = params.value(QStringLiteral("limit")).toInt(50);
        if (limit < 1 || limit > 200) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("limit"));
            return;
        }
        const auto query = params.value(QStringLiteral("query")).toString();
        QJsonArray records;
        qsizetype next = offset;
        for (; next < snapshot.records.size() && records.size() < limit; ++next) {
            const auto& record = snapshot.records[next];
            if (query.isEmpty() || record.id.contains(query, Qt::CaseInsensitive) ||
                record.createdUtc.toString(Qt::ISODate).contains(query))
                records.append(historyJson(record));
        }
        finish({{QStringLiteral("records"), records},
                {QStringLiteral("revision"), static_cast<qint64>(snapshot.revision)},
                {QStringLiteral("next_cursor"),
                 next < snapshot.records.size()
                     ? QString::number(snapshot.revision) + u':' + QString::number(next)
                     : QString()}});
    } else if (method == u"snow_shot_history_get" || method == u"snow_shot_history_action") {
        const auto snapshot = ports.storage->captureHistory().recordsSnapshot();
        const auto id = params.value(QStringLiteral("history_id")).toString();
        const auto it = std::find_if(snapshot.records.cbegin(), snapshot.records.cend(),
                                     [&](const auto& record) { return record.id == id; });
        if (it == snapshot.records.cend()) {
            error(QStringLiteral("not_found"));
            return;
        }
        if (method == u"snow_shot_history_action") {
            if (!ports.historyAction ||
                !ports.historyAction(id, params.value(QStringLiteral("action")).toString())) {
                error(QStringLiteral("busy"));
                return;
            }
            finish({{QStringLiteral("accepted"), true}});
            return;
        }
        auto metadata = historyJson(*it);
        metadata.insert(QStringLiteral("revision"), static_cast<qint64>(snapshot.revision));
        if (!params.value(QStringLiteral("include_image")).toBool()) {
            finish(metadata);
            return;
        }
        auto* repository = &ports.storage->captureHistory();
        background(
            request,
            [repository, record = *it, request, metadata] {
                const auto image = repository->loadResultImage(record);
                if (!image || image->isNull())
                    return failure(request, QStringLiteral("image_unavailable"));
                auto response = reply(request, metadata);
                QBuffer buffer(&response.attachment);
                if (!buffer.open(QIODevice::WriteOnly) || !image->save(&buffer, "PNG"))
                    return failure(request, QStringLiteral("output_failed"));
                if (response.attachment.size() > 60 * 1024 * 1024)
                    return failure(request, QStringLiteral("output_too_large"));
                response.attachmentMime = QStringLiteral("image/png");
                return response;
            },
            [this, request, completion = std::move(completion)](auto response) mutable {
                if (response.ok && !response.attachment.isEmpty()) {
                    const auto artifact = ports.artifactWriter
                                              ? ports.artifactWriter(request.connectionId,
                                                                     std::move(response.attachment),
                                                                     response.attachmentMime)
                                              : QJsonObject();
                    if (artifact.isEmpty())
                        response = failure(request, QStringLiteral("capacity_exceeded"));
                    else {
                        response.attachment.clear();
                        response.attachmentMime.clear();
                        response.result.insert(QStringLiteral("artifact"), artifact);
                    }
                }
                completion(std::move(response));
            });
    } else if (method == u"snow_shot_history_delete" || method == u"snow_shot_history_clear") {
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        storageJob(
            request,
            [&] {
                return ports.storage->captureHistory().removeIfRevision(
                    {params.value(QStringLiteral("history_id")).toString()}, *expected,
                    method == u"snow_shot_history_clear");
            },
            std::move(completion));
    } else if (method == u"snow_shot_configuration_export") {
        QString path;
        if (!ScreenshotMcpSession::validateOutputPath(
                params.value(QStringLiteral("path")).toString(), &path)) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("path"));
            return;
        }
        const auto values = configuration().snapshot();
        background(
            request,
            [request, path, values] {
                const auto result = storage::ConfigurationArchive::write(
                    path, values, storage::ConfigurationStore::currentSchemaVersion(), true);
                if (!result.isEmpty())
                    return failure(request, QStringLiteral("output_failed"));
                return reply(request, {{QStringLiteral("path"), path},
                                       {QStringLiteral("credentials_redacted"), true}});
            },
            std::move(completion), false);
    } else if (method == u"snow_shot_configuration_import") {
        const QFileInfo file(params.value(QStringLiteral("path")).toString());
        if (!file.isAbsolute() || !file.isFile() || file.isSymLink() || !expected) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("path"));
            return;
        }
        if (admittedWork >= 2) {
            error(QStringLiteral("queue_full"));
            return;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto id =
            ports.jobs->start(request.connectionId, method, [cancel] { cancel->store(true); });
        if (id.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        ++admittedWork;
        auto* watcher = new QFutureWatcher<storage::ConfigurationArchiveReadResult>(&q);
        QObject::connect(
            watcher, &QFutureWatcher<storage::ConfigurationArchiveReadResult>::finished, &q,
            [this, watcher, cancel, id, revision = *expected] {
                auto read = watcher->result();
                watcher->deleteLater();
                --admittedWork;
                if (cancel->load() || stopping)
                    return;
                if (!read.isValid()) {
                    ports.jobs->fail(id, QStringLiteral("invalid_archive"));
                    return;
                }
                if (!ports.jobs->setCancellation(id, {}))
                    return;
                bool conflict = false;
                std::shared_future<storage::StorageResult> runtimeCompletion;
                const bool ok = configuration().mutateIfRevision(
                    revision,
                    [&] {
                        read.preserveOmittedCredentials(configuration().snapshot());
                        return ports.settings->importConfigurationSnapshot(
                            read.values, read.schemaVersion, &runtimeCompletion);
                    },
                    &conflict);
                if (runtimeCompletion.valid()) {
                    ++admittedWork;
                    auto* runtimeWatcher = new QFutureWatcher<storage::StorageResult>(&q);
                    QObject::connect(
                        runtimeWatcher, &QFutureWatcher<storage::StorageResult>::finished, &q,
                        [this, runtimeWatcher, id, ok] {
                            const auto result = runtimeWatcher->result();
                            runtimeWatcher->deleteLater();
                            --admittedWork;
                            if (stopping)
                                return;
                            if (ok && result.success)
                                ports.jobs->complete(
                                    id, {{QStringLiteral("revision"),
                                          static_cast<qint64>(configuration().revision())}});
                            else
                                ports.jobs->fail(id, QStringLiteral("storage_failed"));
                        });
                    runtimeWatcher->setFuture(runMcpWork(
                        &workers, [runtimeCompletion] { return runtimeCompletion.get(); }));
                } else if (ok)
                    ports.jobs->complete(id, {{QStringLiteral("revision"),
                                               static_cast<qint64>(configuration().revision())}});
                else
                    ports.jobs->fail(id, conflict ? QStringLiteral("stale_revision")
                                                  : QStringLiteral("storage_failed"));
            });
        watcher->setFuture(runMcpWork(&workers, [path = file.absoluteFilePath()] {
            return storage::ConfigurationArchive::read(path);
        }));
        finish({{QStringLiteral("job_id"), id}});
    } else if (method == u"snow_shot_settings_action" || method == u"snow_shot_storage_cleanup") {
        settings::SettingsActionBinding binding;
        if (method == u"snow_shot_settings_action") {
            const auto* field = ports.settings->registry().field(
                params.value(QStringLiteral("field_id")).toString());
            const auto* action =
                field && field->definition
                    ? std::get_if<settings::SettingsActionDefinition>(&field->definition->payload)
                    : nullptr;
            if (!action) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("field_id"));
                return;
            }
            binding = action->binding;
        } else {
            const auto kind = params.value(QStringLiteral("kind")).toString();
            if (kind == u"history")
                binding = settings::SettingsActionBinding::ClearCaptureHistory;
            else if (kind == u"pinned")
                binding = settings::SettingsActionBinding::ClearPinnedHistory;
            else if (kind == u"thumbnail_cache")
                binding = settings::SettingsActionBinding::ClearThumbnailCache;
            else if (kind == u"temporary_files")
                binding = settings::SettingsActionBinding::ClearRecordingTemp;
            else {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("kind"));
                return;
            }
        }
        if (binding == settings::SettingsActionBinding::ExportConfiguration ||
            binding == settings::SettingsActionBinding::ImportConfiguration) {
            auto forwarded = request;
            forwarded.method = binding == settings::SettingsActionBinding::ExportConfiguration
                                   ? QStringLiteral("snow_shot_configuration_export")
                                   : QStringLiteral("snow_shot_configuration_import");
            handle(forwarded, std::move(completion));
            return;
        }
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        if (configuration().revision() != *expected) {
            error(QStringLiteral("stale_revision"));
            return;
        }
        const auto actionState = ports.settings->actionState(binding);
        if (!actionState.enabled || actionState.busy) {
            error(QStringLiteral("busy"));
            return;
        }
        if (binding == settings::SettingsActionBinding::RestartAsAdministrator) {
            if (!ports.restartAllowed || !ports.restartAllowed()) {
                error(QStringLiteral("busy"));
                return;
            }
            auto response = reply(request, {{QStringLiteral("accepted"), true}});
            response.afterSend = [guard = QPointer<McpApplicationService>(&q), binding,
                                  revision = *expected] {
                if (guard)
                    guard->m_impl->configuration().mutateIfRevision(revision, [&] {
                        return guard->m_impl->ports.settings->triggerAction(binding);
                    });
            };
            completion(std::move(response));
            return;
        }
        if (binding == settings::SettingsActionBinding::OpenLoginItemSettings) {
            change([&] { return ports.settings->triggerAction(binding); });
            return;
        }
        const auto id = ports.jobs->start(request.connectionId, method);
        if (id.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        auto* observation = new QObject(&q);
        const bool cache = binding == settings::SettingsActionBinding::ClearThumbnailCache ||
                           binding == settings::SettingsActionBinding::ClearRecordingTemp;
        const auto observe = [this, observation, id, binding] {
            if (ports.settings->actionState(binding).busy)
                return;
            const auto status = ports.storage->status();
            const bool failed = binding == settings::SettingsActionBinding::ClearCaptureHistory
                                    ? !status.lastHistoryError.isEmpty()
                                : binding == settings::SettingsActionBinding::ClearPinnedHistory
                                    ? !status.lastPinnedError.isEmpty()
                                    : false;
            if (failed)
                ports.jobs->fail(id, QStringLiteral("storage_failed"));
            else
                ports.jobs->complete(id, {{QStringLiteral("completed"), true}});
            observation->deleteLater();
        };
        if (binding == settings::SettingsActionBinding::CopyTodayLog) {
            QObject::connect(
                ports.settings, &settings::SettingsRuntimeSession::actionFinished, observation,
                [this, observation, id, binding](auto action, bool success, const QString&) {
                    if (action != binding)
                        return;
                    if (success)
                        ports.jobs->complete(id, {{QStringLiteral("completed"), true}});
                    else
                        ports.jobs->fail(id, QStringLiteral("output_failed"));
                    observation->deleteLater();
                });
        } else if (cache) {
            QObject::connect(ports.storage, &storage::ApplicationStorage::cacheClearFinished,
                             observation, [this, observation, id](auto, bool success) {
                                 if (success)
                                     ports.jobs->complete(id,
                                                          {{QStringLiteral("completed"), true}});
                                 else
                                     ports.jobs->fail(id, QStringLiteral("storage_failed"));
                                 observation->deleteLater();
                             });
        } else {
            QObject::connect(ports.storage, &storage::ApplicationStorage::storageStatusChanged,
                             observation, [observe](const auto&) { observe(); });
        }
        bool conflict = false;
        const bool accepted = configuration().mutateIfRevision(
            *expected,
            [&] {
                return ports.settings->triggerAction(
                    binding, params.value(QStringLiteral("path")).toString());
            },
            &conflict);
        if (!accepted) {
            delete observation;
            ports.jobs->fail(id, conflict ? QStringLiteral("stale_revision")
                                          : QStringLiteral("operation_failed"));
            error(conflict ? QStringLiteral("stale_revision") : QStringLiteral("operation_failed"));
            return;
        }
        finish({{QStringLiteral("job_id"), id},
                {QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
        if (binding != settings::SettingsActionBinding::CopyTodayLog && !cache)
            observe();
    } else if (method == u"snow_shot_templates_list") {
        const auto kind = params.value(QStringLiteral("kind")).toString();
        QJsonArray items;
        if (kind == u"drawing") {
            for (const auto& item : storage::DrawTemplateSettings().templates())
                items.append(QJsonObject{
                    {QStringLiteral("name"), item.name},
                    {QStringLiteral("payload"), QJsonDocument::fromJson(item.payload).object()}});
        } else if (kind == u"watermark") {
            for (const auto& item : storage::WatermarkTemplateSettings().templates())
                items.append(QJsonObject{{QStringLiteral("name"), item.name},
                                         {QStringLiteral("text"), item.value}});
        } else {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("kind"));
            return;
        }
        finish({{QStringLiteral("templates"), items},
                {QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
    } else if (method == u"snow_shot_templates_update") {
        const auto kind = params.value(QStringLiteral("kind")).toString();
        const auto items = params.value(QStringLiteral("templates")).toArray();
        if (items.size() > 128) {
            error(QStringLiteral("invalid_parameters"));
            return;
        }
        QVector<storage::DrawTemplate> drawings;
        QVector<storage::WatermarkTemplate> watermarks;
        QSet<QString> names;
        for (const auto& value : items) {
            const auto item = value.toObject();
            const auto name = item.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty() || names.contains(name) || name.size() > 128) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("templates"));
                return;
            }
            names.insert(name);
            if (kind == u"drawing" && item.value(QStringLiteral("payload")).isObject())
                drawings.append(
                    {name, QJsonDocument(item.value(QStringLiteral("payload")).toObject())
                               .toJson(QJsonDocument::Compact)});
            else if (kind == u"watermark" && item.value(QStringLiteral("text")).isString())
                watermarks.append({name, item.value(QStringLiteral("text")).toString()});
            else {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("templates"));
                return;
            }
        }
        if (kind != u"drawing" && kind != u"watermark") {
            error(QStringLiteral("invalid_parameters"));
            return;
        }
        change([&] {
            return kind == u"drawing"
                       ? storage::DrawTemplateSettings().setTemplates(drawings)
                       : storage::WatermarkTemplateSettings().setTemplates(watermarks);
        });
    } else if (method == u"snow_shot_translation_catalog") {
        if (!ports.translation) {
            error(QStringLiteral("unavailable"));
            return;
        }
        ports.translation->refreshModels();
        QJsonArray languages, models;
        for (const auto& language : translation::translationLanguages())
            languages.append(QJsonObject{
                {QStringLiteral("code"), QString::fromUtf8(language.code)},
                {QStringLiteral("name"),
                 translation::translationLanguageName(QString::fromUtf8(language.code))}});
        for (const auto& model : ports.translation->models())
            models.append(QJsonObject{{QStringLiteral("id"), model.id},
                                      {QStringLiteral("name"), model.name}});
        const auto preferences = ports.translation->preferences();
        finish({{QStringLiteral("models"), models},
                {QStringLiteral("languages"), languages},
                {QStringLiteral("loading"), ports.translation->loadingModels()},
                {QStringLiteral("preferences"),
                 QJsonObject{{QStringLiteral("source_language"), preferences.sourceLanguage},
                             {QStringLiteral("target_language"), preferences.targetLanguage},
                             {QStringLiteral("model_id"), preferences.modelId}}}});
    } else if (method == u"snow_shot_translation_start") {
        if (!ports.translation || !ports.jobs) {
            error(QStringLiteral("unavailable"));
            return;
        }
        auto input = params;
        if (params.contains(QStringLiteral("retry_job_id"))) {
            for (const auto& key :
                 {QStringLiteral("source"), QStringLiteral("texts"),
                  QStringLiteral("source_language"), QStringLiteral("target_language"),
                  QStringLiteral("model_id")}) {
                if (params.contains(key)) {
                    error(QStringLiteral("invalid_parameters"), key);
                    return;
                }
            }
            const auto previousId = params.value(QStringLiteral("retry_job_id")).toString();
            const auto previous = ports.jobs->get(request.connectionId, previousId);
            if (!previous || previous->value(QStringLiteral("kind")) != method) {
                error(QStringLiteral("job_not_found"));
                return;
            }
            if (previous->value(QStringLiteral("status")) == QStringLiteral("running")) {
                error(QStringLiteral("invalid_state"));
                return;
            }
            const auto retained = ports.jobs->input(request.connectionId, previousId);
            if (!retained) {
                error(QStringLiteral("retry_unavailable"));
                return;
            }
            input = *retained;
        }
        QStringList texts;
        const bool selected = input.value(QStringLiteral("source")).toString() == u"selection";
        if ((!selected && (!stringArray(input.value(QStringLiteral("texts")), &texts) ||
                           texts.isEmpty() || texts.size() > 256)) ||
            (selected && input.contains(QStringLiteral("texts")))) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("texts"));
            return;
        }
        if (selected && !ports.selectedText) {
            error(QStringLiteral("unsupported"));
            return;
        }
        auto preferences = ports.translation->preferences();
        if (input.contains(QStringLiteral("source_language")))
            preferences.sourceLanguage = input.value(QStringLiteral("source_language")).toString();
        if (input.contains(QStringLiteral("target_language")))
            preferences.targetLanguage = input.value(QStringLiteral("target_language")).toString();
        if (input.contains(QStringLiteral("model_id")))
            preferences.modelId = input.value(QStringLiteral("model_id")).toString();
        auto canceled = std::make_shared<bool>(false);
        auto captureCancellation = std::make_shared<std::function<void()>>();
        const auto id =
            ports.jobs->start(request.connectionId, method, [canceled, captureCancellation] {
                *canceled = true;
                if (*captureCancellation)
                    (*captureCancellation)();
            });
        if (id.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        finish({{QStringLiteral("job_id"), id}});
        if (selected) {
            *captureCancellation =
                ports.selectedText([guard = QPointer<McpApplicationService>(&q), id, canceled,
                                    preferences](QString text, QString captureError) {
                    if (!guard || *canceled)
                        return;
                    auto& self = *guard->m_impl;
                    if (!captureError.isEmpty() || text.isEmpty()) {
                        self.ports.jobs->fail(id, captureError.isEmpty()
                                                      ? QStringLiteral("no_selection")
                                                      : captureError);
                        return;
                    }
                    self.translate(id, {text}, preferences);
                });
        } else
            translate(id, texts, preferences);
    } else {
        error(QStringLiteral("method_not_found"));
    }
}
} // namespace snow_shot::app::mcp
