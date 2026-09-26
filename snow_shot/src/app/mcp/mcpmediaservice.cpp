#include "snow_shot/app/mcp/mcpmediaservice.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <cmath>

namespace snow_shot::app::mcp {
namespace {
const QStringList methods{
    QStringLiteral("snow_shot_recording_state"),   QStringLiteral("snow_shot_recording_start"),
    QStringLiteral("snow_shot_recording_control"), QStringLiteral("snow_shot_pinned_list"),
    QStringLiteral("snow_shot_pinned_get"),        QStringLiteral("snow_shot_pinned_create"),
    QStringLiteral("snow_shot_pinned_replace"),    QStringLiteral("snow_shot_pinned_update"),
    QStringLiteral("snow_shot_pinned_action"),     QStringLiteral("snow_shot_pinned_export"),
    QStringLiteral("snow_shot_pinned_edit"),       QStringLiteral("snow_shot_group_list"),
    QStringLiteral("snow_shot_group_update")};
QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
bool parseRect(const QJsonValue& value, QRect* rect) {
    const auto a = value.toArray();
    if (a.size() != 4)
        return false;
    for (int i = 0; i < 4; ++i) {
        const double n = a[i].toDouble(1e12);
        if (!a[i].isDouble() || !std::isfinite(n) || std::floor(n) != n ||
            n < (i < 2 ? -1000000 : 1) || n > (i < 2 ? 1000000 : 32768))
            return false;
    }
    *rect = QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
    return true;
}
} // namespace

class McpMediaService::Impl {
  public:
    Impl(McpMediaService& owner, ScreenshotController& controller,
         presentation::PinnedWindowGroupManager& groups)
        : q(owner), controller(controller), groups(groups) {}
    McpMediaService& q;
    ScreenshotController& controller;
    presentation::PinnedWindowGroupManager& groups;
    QPointer<ScreenRecordingController> recording;
    QString recordingId;
    quint64 recordingOwner = 0;
    bool stopped = false;
    ArtifactWriter artifactWriter;
    FileArtifactWriter fileArtifactWriter;
    struct RecordingArtifact {
        QString recordingId;
        QString path;
        QString status;
        QJsonObject descriptor;
        QString error;
    };
    QHash<quint64, RecordingArtifact> recordingArtifacts;
    quint64 recordingArtifactRevision = 0;
    struct Version {
        QByteArray fingerprint;
        quint64 revision = 0;
    };
    QHash<QString, Version> versions;
    quint64 nextRevision = 0;
    struct Pending {
        quint64 owner = 0;
        ScreenshotExportJobHandle job;
        QString pinId;
    };
    QHash<QString, Pending> pending;
    QHash<quint64, QSet<QString>> recognitionOwners;
    QHash<quint64, QList<std::shared_ptr<ScreenshotExportArtifact>>> exports;
    QHash<quint64, QList<ScreenshotClipboardCommitHandle>> clipboardCommits;
    QHash<QString, std::function<void()>> cancellations;
    struct Cached {
        QByteArray fingerprint;
        ScreenshotMcpResponse response;
        qsizetype bytes = 0;
    };
    QHash<QString, Cached> replay;
    struct InflightReplay {
        QByteArray fingerprint;
        QList<ScreenshotMcpServer::Completion> waiters;
    };
    QHash<QString, InflightReplay> inflightReplay;
    QStringList replayOrder;
    qsizetype replayBytes = 0;

    quint64 observe(const QString& key, QJsonObject state) {
        state.remove(QStringLiteral("duration_ms"));
        const auto fingerprint = QCryptographicHash::hash(
            QJsonDocument(state).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        if (!versions.contains(key) && versions.size() >= 512)
            versions.erase(versions.begin());
        auto& v = versions[key];
        if (v.revision == 0 || v.fingerprint != fingerprint) {
            v.fingerprint = fingerprint;
            v.revision = ++nextRevision;
        }
        return v.revision;
    }
    QJsonObject versioned(const QString& key, QJsonObject state) {
        QJsonObject identity{{QStringLiteral("revision"), state.value(QStringLiteral("revision"))},
                             {QStringLiteral("repository_revision"),
                              state.value(QStringLiteral("repository_revision"))},
                             {QStringLiteral("artifact_revision"),
                              state.value(QStringLiteral("artifact_revision"))}};
        state.insert(QStringLiteral("revision"), static_cast<qint64>(observe(key, identity)));
        return state;
    }
    void ensureRecording() {
        if (recording)
            return;
        recording = controller.automationRecordingController();
        if (recording)
            QObject::connect(recording, &ScreenRecordingController::finalized, &q, [this] {
                if (!stopped && recordingOwner)
                    static_cast<void>(recordingState(recordingOwner));
            });
    }
    QJsonObject recordingState(quint64 artifactOwner) {
        ensureRecording();
        QJsonObject state = recording
                                ? recording->automationState()
                                : QJsonObject{{QStringLiteral("state"), QStringLiteral("idle")},
                                              {QStringLiteral("open"), false}};
        if (state.value(QStringLiteral("open")).toBool() && recordingId.isEmpty())
            recordingId = uuid();
        if (!state.value(QStringLiteral("automated")).toBool())
            recordingOwner = 0;
        state.insert(QStringLiteral("recording_id"), recordingId);
        const auto path = state.value(QStringLiteral("path")).toString();
        if (artifactOwner && state.value(QStringLiteral("finalized")).toBool() && !path.isEmpty()) {
            auto& artifact = recordingArtifacts[artifactOwner];
            if (artifact.recordingId != recordingId || artifact.path != path) {
                artifact = {recordingId,
                            path,
                            fileArtifactWriter ? QStringLiteral("pending")
                                               : QStringLiteral("unavailable"),
                            {},
                            fileArtifactWriter ? QString() : QStringLiteral("unavailable")};
                ++recordingArtifactRevision;
                if (fileArtifactWriter) {
                    const auto format = state.value(QStringLiteral("format")).toString();
                    const auto mime = format == QStringLiteral("mp4")
                                          ? QStringLiteral("video/mp4")
                                          : QStringLiteral("image/") + format;
                    const QPointer<McpMediaService> guard(&q);
                    fileArtifactWriter(artifactOwner, path, mime,
                                       [guard, artifactOwner, id = recordingId,
                                        path](QJsonObject descriptor, QString error) {
                                           if (!guard || guard->m_impl->stopped)
                                               return;
                                           auto& impl = *guard->m_impl;
                                           auto found = impl.recordingArtifacts.find(artifactOwner);
                                           if (found == impl.recordingArtifacts.end() ||
                                               found->recordingId != id || found->path != path)
                                               return;
                                           found->status = descriptor.isEmpty()
                                                               ? QStringLiteral("unavailable")
                                                               : QStringLiteral("ready");
                                           found->descriptor = std::move(descriptor);
                                           found->error = std::move(error);
                                           ++impl.recordingArtifactRevision;
                                       });
                }
            }
            state.insert(QStringLiteral("artifact_status"), artifact.status);
            if (!artifact.descriptor.isEmpty())
                state.insert(QStringLiteral("artifact"), artifact.descriptor);
            else if (artifact.status == QStringLiteral("unavailable"))
                state.insert(QStringLiteral("artifact_error"), artifact.error);
        }
        state.insert(QStringLiteral("artifact_revision"),
                     static_cast<qint64>(recordingArtifactRevision));
        return versioned(QStringLiteral("recording"), state);
    }
    QJsonObject groupState() {
        QJsonArray list;
        for (const auto& group : groups.groups()) {
            const auto counts = groups.windowCounts(group.id);
            list.append(QJsonObject{{QStringLiteral("id"), group.id},
                                    {QStringLiteral("name"), group.name},
                                    {QStringLiteral("built_in"), group.builtIn},
                                    {QStringLiteral("count"), counts.total},
                                    {QStringLiteral("open_count"), counts.nonIgnored}});
        }
        return versioned(
            QStringLiteral("groups"),
            {{QStringLiteral("revision"), static_cast<qint64>(groups.automationRevision())},
             {QStringLiteral("repository_revision"),
              static_cast<qint64>(
                  storage::ApplicationStorage::instance().pinnedWindows().revision())},
             {QStringLiteral("groups"), list},
             {QStringLiteral("active_group_id"), groups.activeGroupId()}});
    }
    QJsonObject pinState(const QString& id) {
        if (auto* window = groups.liveWindow(id))
            return versioned(QStringLiteral("pin:") + id, window->automationState());
        for (const auto& summary :
             storage::ApplicationStorage::instance().pinnedWindows().summaries())
            if (summary.id == id)
                return versioned(
                    QStringLiteral("pin:") + id,
                    {{QStringLiteral("id"), id},
                     {QStringLiteral("revision"),
                      static_cast<qint64>(
                          storage::ApplicationStorage::instance().pinnedWindows().revision())},
                     {QStringLiteral("group_id"), summary.groupId},
                     {QStringLiteral("visible"), false},
                     {QStringLiteral("open"), false},
                     {QStringLiteral("updated_at"), summary.updatedUtc.toString(Qt::ISODateWithMs)},
                     {QStringLiteral("closed"), summary.ignored}});
        return {};
    }
    using Reply = std::function<void(QJsonObject, QString)>;
    void decode(const ScreenshotMcpRequest& r, Reply reply) {
        if (pending.size() >= 8) {
            reply({}, QStringLiteral("queue_full"));
            return;
        }
        const auto source = r.params.value(QStringLiteral("source")).toObject();
        const auto kind = source.value(QStringLiteral("kind")).toString();
        if (!QStringList{QStringLiteral("file"), QStringLiteral("clipboard"),
                         QStringLiteral("text"), QStringLiteral("html")}
                 .contains(kind)) {
            reply({}, QStringLiteral("invalid_parameters"));
            return;
        }
        const auto path = source.value(QStringLiteral("path")).toString();
        if (kind == QStringLiteral("file") &&
            (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile())) {
            reply({}, QStringLiteral("invalid_path"));
            return;
        }
        const qreal dpr = QGuiApplication::primaryScreen()
                              ? QGuiApplication::primaryScreen()->devicePixelRatio()
                              : 1;
        std::optional<ScreenshotClipboardContentSnapshot> snapshot;
        if (kind == QStringLiteral("clipboard"))
            snapshot = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), dpr);
        else if (kind == QStringLiteral("text") || kind == QStringLiteral("html")) {
            QMimeData mime;
            if (kind == QStringLiteral("html"))
                mime.setHtml(source.value(QStringLiteral("html")).toString());
            mime.setText(source.value(QStringLiteral("text")).toString());
            snapshot = ScreenshotClipboardContentReader::snapshotMimeData(&mime, dpr, Qt::white);
        }
        if (kind != QStringLiteral("file") && !snapshot) {
            reply({}, QStringLiteral("empty_source"));
            return;
        }
        auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
        const QString token = uuid();
        const auto id = r.params.value(QStringLiteral("id")).toString();
        const bool replace = r.method == QStringLiteral("snow_shot_pinned_replace");
        const auto group = r.params.value(QStringLiteral("group_id")).toString();
        if (!group.isEmpty() && !groups.contains(group)) {
            reply({}, QStringLiteral("group_not_found"));
            return;
        }
        const QPointer<McpMediaService> guard(&q);
        auto job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [snapshot = std::move(snapshot), path, kind, content,
             dpr](const ScreenshotExportCancellation& cancel) mutable {
                if (kind == QStringLiteral("file")) {
                    const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
                    if (files.size() != 1)
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Source,
                            QStringLiteral("unsupported_source"));
                    ScreenshotClipboardContentSnapshot file;
                    file.localImage = files.front();
                    file.devicePixelRatio = dpr;
                    snapshot = std::move(file);
                }
                *content = ScreenshotClipboardContentReader::decode(
                    std::move(*snapshot), [&cancel] { return cancel.isCancellationRequested(); });
                return *content ? ScreenshotExportTaskResult{}
                                : ScreenshotExportTaskResult::failure(
                                      ScreenshotExportFailureStage::Source,
                                      QStringLiteral("decode_failed"));
            },
            [this, guard, content, token, id, replace, group, expected = r.expectedRevision,
             reply](ScreenshotExportTaskResult result) {
                if (!guard || !pending.contains(token))
                    return;
                if (!result.succeeded() || !*content) {
                    pending.remove(token);
                    reply({}, result.error);
                    return;
                }
                if (replace) {
                    pending.remove(token);
                    const auto state = pinState(id);
                    if (!expected ||
                        static_cast<quint64>(state.value(QStringLiteral("revision")).toInteger()) !=
                            *expected) {
                        reply(state, QStringLiteral("stale_revision"));
                        return;
                    }
                    auto* window = groups.liveWindow(id);
                    if (!window || !window->automationReplaceContent(std::move(**content)))
                        reply({}, QStringLiteral("replacement_failed"));
                    else
                        reply(pinState(id), {});
                    return;
                }
                QSet<QString> before;
                for (auto* window : groups.liveWindows())
                    before.insert(window->persistenceId());
                const auto created = std::make_shared<QString>();
                if (!controller.mcpPinContent(
                        std::move(**content), [this, guard, created, group, reply, token](bool ok) {
                            if (!guard)
                                return;
                            QTimer::singleShot(0, &q, [this, created, group, reply, ok, token] {
                                if (!pending.contains(token))
                                    return;
                                pending.remove(token);
                                if (!ok || created->isEmpty()) {
                                    reply({}, QStringLiteral("pin_failed"));
                                    return;
                                }
                                if (!group.isEmpty()) {
                                    if (auto* window = groups.liveWindow(*created))
                                        static_cast<void>(groups.moveWindow(window, group));
                                }
                                reply(pinState(*created), {});
                            });
                        })) {
                    pending.remove(token);
                    reply({}, QStringLiteral("pin_failed"));
                    return;
                }
                for (auto* window : groups.liveWindows())
                    if (!before.contains(window->persistenceId())) {
                        *created = window->persistenceId();
                        pending[token].pinId = *created;
                        break;
                    }
            });
        if (!job.isValid()) {
            reply({}, QStringLiteral("queue_full"));
            return;
        }
        pending.insert(token, {r.connectionId, std::move(job), {}});
        cancellations.insert(QString::number(r.connectionId) + u':' + r.requestId,
                             [this, token, reply] {
                                 if (!pending.contains(token))
                                     return;
                                 const auto task = pending.take(token);
                                 task.job.cancel();
                                 if (!task.pinId.isEmpty())
                                     controller.destroyPinnedRecords({task.pinId});
                                 reply({}, QStringLiteral("cancelled"));
                             });
    }
};

McpMediaService::McpMediaService(ScreenshotController& controller,
                                 presentation::PinnedWindowGroupManager& groups, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, controller, groups)) {}
McpMediaService::~McpMediaService() {
    shutdown();
}
bool McpMediaService::handles(const QString& method) const {
    return methods.contains(method);
}

void McpMediaService::setArtifactWriter(ArtifactWriter writer) {
    m_impl->artifactWriter = std::move(writer);
}
void McpMediaService::setFileArtifactWriter(FileArtifactWriter writer) {
    m_impl->fileArtifactWriter = std::move(writer);
}

void McpMediaService::request(const ScreenshotMcpRequest& r,
                              ScreenshotMcpServer::Completion completion) {
    auto& s = *m_impl;
    const QString replayKey = QString::number(r.connectionId) + u':' + r.idempotencyKey;
    const QByteArray fingerprint = QCryptographicHash::hash(
        QJsonDocument(
            QJsonObject{{QStringLiteral("method"), r.method},
                        {QStringLiteral("params"), r.params},
                        {QStringLiteral("revision"),
                         r.expectedRevision ? QJsonValue(static_cast<qint64>(*r.expectedRevision))
                                            : QJsonValue{}}})
            .toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
    if (!r.idempotencyKey.isEmpty() && s.replay.contains(replayKey)) {
        auto cached = s.replay.value(replayKey);
        if (cached.fingerprint == fingerprint) {
            cached.response.requestId = r.requestId;
            completion(std::move(cached.response));
            return;
        }
        ScreenshotMcpResponse response;
        response.requestId = r.requestId;
        response.errorCode = QStringLiteral("idempotency_conflict");
        completion(std::move(response));
        return;
    }
    if (!r.idempotencyKey.isEmpty() && s.inflightReplay.contains(replayKey)) {
        auto& pending = s.inflightReplay[replayKey];
        if (pending.fingerprint == fingerprint) {
            pending.waiters.append([completion = std::move(completion), requestId = r.requestId](
                                       ScreenshotMcpResponse response) mutable {
                response.requestId = requestId;
                completion(std::move(response));
            });
        } else {
            ScreenshotMcpResponse response;
            response.requestId = r.requestId;
            response.errorCode = QStringLiteral("idempotency_conflict");
            completion(std::move(response));
        }
        return;
    }
    if (!r.idempotencyKey.isEmpty())
        s.inflightReplay.insert(replayKey, {fingerprint, {}});
    const QPointer<McpMediaService> guard(this);
    const auto finished = std::make_shared<bool>(false);
    const auto reply = [guard, r, replayKey, fingerprint, finished,
                        completion = std::move(completion)](QJsonObject value, QString error) {
        if (!guard || *finished)
            return;
        *finished = true;
        guard->m_impl->cancellations.remove(QString::number(r.connectionId) + u':' + r.requestId);
        ScreenshotMcpResponse response;
        response.requestId = r.requestId;
        response.ok = error.isEmpty();
        response.result = std::move(value);
        response.errorCode = std::move(error);
        response.errorMessage = response.errorCode;
        if (response.result.contains(QStringLiteral("revision")))
            response.revision =
                static_cast<quint64>(response.result.value(QStringLiteral("revision")).toInteger());
        auto serialized = QJsonDocument(response.result).toJson(QJsonDocument::Compact);
        if (serialized.size() > 1024 * 1024) {
            const auto artifact =
                guard->m_impl->artifactWriter
                    ? guard->m_impl->artifactWriter(r.connectionId, std::move(serialized),
                                                    QStringLiteral("application/json"))
                    : QJsonObject{};
            response.result = {{QStringLiteral("artifact"), artifact}};
            if (response.revision)
                response.result.insert(QStringLiteral("revision"),
                                       static_cast<qint64>(*response.revision));
            if (artifact.isEmpty()) {
                response.ok = false;
                response.errorCode = QStringLiteral("resource_limit");
            }
        }
        if (response.ok && !r.idempotencyKey.isEmpty()) {
            auto& impl = *guard->m_impl;
            const auto bytes = QJsonDocument(response.result).toJson(QJsonDocument::Compact).size();
            constexpr qsizetype maximumReplayBytes = 8 * 1024 * 1024;
            if (bytes <= maximumReplayBytes) {
                if (impl.replay.contains(replayKey)) {
                    impl.replayBytes -= impl.replay.value(replayKey).bytes;
                    impl.replayOrder.removeAll(replayKey);
                }
                impl.replay.insert(replayKey, {fingerprint, response, bytes});
                impl.replayBytes += bytes;
                impl.replayOrder.append(replayKey);
                while (impl.replayOrder.size() > 128 || impl.replayBytes > maximumReplayBytes)
                    impl.replayBytes -= impl.replay.take(impl.replayOrder.takeFirst()).bytes;
            }
        }
        const auto waiters = guard->m_impl->inflightReplay.take(replayKey).waiters;
        for (const auto& waiter : waiters)
            waiter(response);
        completion(std::move(response));
    };
    if (s.stopped) {
        reply({}, QStringLiteral("unavailable"));
        return;
    }
    const auto current = [&](const QJsonObject& state) {
        if (!r.expectedRevision) {
            reply(state, QStringLiteral("revision_required"));
            return false;
        }
        if (*r.expectedRevision !=
            static_cast<quint64>(state.value(QStringLiteral("revision")).toInteger())) {
            reply(state, QStringLiteral("stale_revision"));
            return false;
        }
        return true;
    };
    const auto& p = r.params;
    if (r.method == QStringLiteral("snow_shot_recording_state")) {
        reply(s.recordingState(r.connectionId), {});
        return;
    }
    if (r.method == QStringLiteral("snow_shot_recording_start")) {
        QRect region;
        if (!parseRect(p.value(QStringLiteral("region")), &region)) {
            reply({}, QStringLiteral("invalid_region"));
            return;
        }
        s.ensureRecording();
        QString error;
        if (!s.recording || !s.recording->startAutomation(
                                region, p.value(QStringLiteral("options")).toObject(), &error)) {
            reply(s.recordingState(r.connectionId),
                  error.isEmpty() ? QStringLiteral("unavailable") : error);
            return;
        }
        s.recordingId = uuid();
        s.recordingOwner = r.connectionId;
        s.recordingArtifacts.clear();
        ++s.recordingArtifactRevision;
        reply(s.recordingState(r.connectionId), {});
        return;
    }
    if (r.method == QStringLiteral("snow_shot_recording_control")) {
        if (!s.recording || p.value(QStringLiteral("recording_id")).toString() != s.recordingId) {
            reply({}, QStringLiteral("recording_not_found"));
            return;
        }
        if (s.recordingOwner && s.recordingOwner != r.connectionId) {
            reply({}, QStringLiteral("not_owner"));
            return;
        }
        if (!current(s.recordingState(r.connectionId)))
            return;
        QString error;
        const bool ok =
            s.recording->controlAutomation(p.value(QStringLiteral("action")).toString(),
                                           p.value(QStringLiteral("payload")).toObject(), &error);
        reply(s.recordingState(r.connectionId), ok ? QString() : error);
        return;
    }
    if (r.method == QStringLiteral("snow_shot_group_list")) {
        reply(s.groupState(), {});
        return;
    }
    if (r.method == QStringLiteral("snow_shot_group_update")) {
        if (!current(s.groupState()))
            return;
        const auto action = p.value(QStringLiteral("action")).toString();
        const auto id = p.value(QStringLiteral("group_id")).toString();
        bool ok = false;
        QString created;
        if (action == QStringLiteral("create")) {
            const auto group = s.groups.createGroup(p.value(QStringLiteral("name")).toString());
            ok = group.has_value();
            if (group)
                created = *group;
        } else if (action == QStringLiteral("activate"))
            ok = s.groups.setActiveGroup(id);
        else if (action == QStringLiteral("move")) {
            const auto pin = p.value(QStringLiteral("id")).toString();
            if (auto* window = s.groups.liveWindow(pin))
                ok = s.groups.moveWindow(window, id);
            else
                ok = storage::ApplicationStorage::instance()
                         .pinnedWindows()
                         .setRecordGroup(pin, id)
                         .success;
        } else if (action == QStringLiteral("delete"))
            ok = s.groups.deleteSpecifiedGroup(id);
        else if (action == QStringLiteral("delete_empty"))
            ok = s.groups.deleteEmptyGroups();
        auto state = s.groupState();
        if (!created.isEmpty())
            state.insert(QStringLiteral("created_id"), created);
        reply(state, ok ? QString() : QStringLiteral("action_unavailable"));
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_list")) {
        const int offset = p.value(QStringLiteral("offset")).toInt(0);
        const int limit = p.value(QStringLiteral("limit")).toInt(50);
        if (offset < 0 || limit < 1 || limit > 200) {
            reply({}, QStringLiteral("invalid_parameters"));
            return;
        }
        QJsonArray list;
        QSet<QString> found;
        for (const auto& summary :
             storage::ApplicationStorage::instance().pinnedWindows().summaries()) {
            found.insert(summary.id);
            list.append(QJsonObject{
                {QStringLiteral("id"), summary.id},
                {QStringLiteral("group_id"), summary.groupId},
                {QStringLiteral("open"), s.groups.hasWindow(summary.id)},
                {QStringLiteral("closed"), summary.ignored},
                {QStringLiteral("updated_at"), summary.updatedUtc.toString(Qt::ISODateWithMs)}});
        }
        for (auto* window : s.groups.liveWindows())
            if (!found.contains(window->persistenceId()))
                list.append(QJsonObject{{QStringLiteral("id"), window->persistenceId()},
                                        {QStringLiteral("group_id"), window->groupId()},
                                        {QStringLiteral("open"), true}});
        const auto revision = s.observe(
            QStringLiteral("pins"),
            {{QStringLiteral("revision"), static_cast<qint64>(s.groups.automationRevision())},
             {QStringLiteral("repository_revision"),
              static_cast<qint64>(
                  storage::ApplicationStorage::instance().pinnedWindows().revision())}});
        QJsonArray page;
        for (qsizetype index = offset; index < list.size() && page.size() < limit; ++index)
            page.append(list[index]);
        reply({{QStringLiteral("windows"), page},
               {QStringLiteral("total"), static_cast<qint64>(list.size())},
               {QStringLiteral("revision"), static_cast<qint64>(revision)},
               {QStringLiteral("next_offset"),
                offset + page.size() < list.size()
                    ? QJsonValue(static_cast<qint64>(offset + page.size()))
                    : QJsonValue{}}},
              {});
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_create")) {
        s.decode(r, reply);
        return;
    }
    const auto id = p.value(QStringLiteral("id")).toString();
    auto state = s.pinState(id);
    const auto action = p.value(QStringLiteral("action")).toString();
    if (r.method == QStringLiteral("snow_shot_pinned_action") &&
        action == QStringLiteral("restore_last")) {
        if (!current(s.groupState()))
            return;
        s.controller.restoreLastClosedPinnedWindow();
        reply({{QStringLiteral("accepted"), true}}, {});
        return;
    }
    if (state.isEmpty()) {
        reply({}, QStringLiteral("pinned_not_found"));
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_get")) {
        reply(state, {});
        return;
    }
    if (!current(state))
        return;
    if (r.method == QStringLiteral("snow_shot_pinned_action") &&
        (action == QStringLiteral("show") || action == QStringLiteral("destroy"))) {
        if (action == QStringLiteral("show"))
            s.controller.showPinnedRecord(id);
        else
            s.controller.destroyPinnedRecords({id});
        reply({{QStringLiteral("accepted"), true}, {QStringLiteral("id"), id}}, {});
        return;
    }
    auto* window = s.groups.liveWindow(id);
    if (!window) {
        reply(state, QStringLiteral("window_closed"));
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_replace")) {
        s.decode(r, reply);
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_update")) {
        QString error;
        const bool ok =
            window->automationUpdate(p.value(QStringLiteral("properties")).toObject(), &error);
        reply(s.pinState(id), ok ? QString() : error);
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_action")) {
        const bool ok = window->automationAction(action);
        reply({{QStringLiteral("accepted"), ok}, {QStringLiteral("id"), id}},
              ok ? QString() : QStringLiteral("invalid_parameters"));
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_edit")) {
        const bool startsJob = action == QStringLiteral("recognize") ||
                               action == QStringLiteral("translate") ||
                               action == QStringLiteral("auto_filter");
        if (startsJob && (state.value(QStringLiteral("recognition"))
                              .toObject()
                              .value(QStringLiteral("busy"))
                              .toBool() ||
                          state.value(QStringLiteral("auto_filter"))
                              .toObject()
                              .value(QStringLiteral("busy"))
                              .toBool())) {
            reply(state, QStringLiteral("busy"));
            return;
        }
        QString error;
        auto result =
            window->automationEdit(action, p.value(QStringLiteral("payload")).toObject(), &error);
        if (error.isEmpty() && startsJob) {
            for (auto& owned : s.recognitionOwners)
                owned.remove(id);
            s.recognitionOwners[r.connectionId].insert(id);
        }
        auto updatedState = s.pinState(id);
        updatedState.insert(QStringLiteral("result"), result);
        reply(updatedState, error);
        return;
    }
    if (r.method == QStringLiteral("snow_shot_pinned_export")) {
        if (s.cancellations.size() >= 8) {
            reply({}, QStringLiteral("queue_full"));
            return;
        }
        const auto output = p.value(QStringLiteral("output")).toString();
        if (output != QStringLiteral("copy") && output != QStringLiteral("save")) {
            reply({}, QStringLiteral("invalid_parameters"));
            return;
        }
        const bool original = p.value(QStringLiteral("original")).toBool();
        if (output == QStringLiteral("copy")) {
            if (auto mime = window->automationClipboardMimeData(original)) {
                auto commit = ScreenshotClipboardService::commitMimeData(
                    QApplication::clipboard(), this, mime.release(),
                    [reply](ScreenshotClipboardCommitResult result) {
                        reply({{QStringLiteral("copied"), result.succeeded()}},
                              result.succeeded() ? QString() : QStringLiteral("clipboard_failed"));
                    });
                if (!commit.isValid()) {
                    reply({}, QStringLiteral("clipboard_failed"));
                    return;
                }
                s.cancellations.insert(QString::number(r.connectionId) + u':' + r.requestId,
                                       [commit, reply] {
                                           commit.cancel();
                                           reply({}, QStringLiteral("cancelled"));
                                       });
                auto& commits = s.clipboardCommits[r.connectionId];
                commits.append(std::move(commit));
                while (commits.size() > 32)
                    commits.removeFirst();
                return;
            }
        } else if (!original) {
            if (const auto snapshot = window->automationFileSnapshot()) {
                const auto path = p.value(QStringLiteral("path")).toString();
                if (!QFileInfo(path).isAbsolute() ||
                    !QFileInfo(QFileInfo(path).absolutePath()).isDir()) {
                    reply({}, QStringLiteral("invalid_path"));
                    return;
                }
                const auto job = ScreenshotExportCoordinator::shared().submit(
                    this, ScreenshotExportCoordinator::Priority::Foreground,
                    [snapshot = *snapshot, path](const ScreenshotExportCancellation& cancellation) {
                        if (cancellation.isCancellationRequested())
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Cancelled,
                                QStringLiteral("cancelled"));
                        const auto saved =
                            ScreenshotRecognitionFileExport::saveToPath(snapshot, path);
                        ScreenshotExportTaskResult result;
                        result.savedPath = saved.path;
                        if (!saved.succeeded())
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::File, saved.error);
                        return result;
                    },
                    [reply](ScreenshotExportTaskResult result) {
                        reply({{QStringLiteral("path"), result.savedPath}},
                              result.succeeded() ? QString() : QStringLiteral("export_failed"));
                    });
                if (!job.isValid()) {
                    reply({}, QStringLiteral("queue_full"));
                    return;
                }
                s.cancellations.insert(QString::number(r.connectionId) + u':' + r.requestId,
                                       [job, reply] {
                                           job.cancel();
                                           reply({}, QStringLiteral("cancelled"));
                                       });
                return;
            }
        }
        auto artifact = window->automationArtifact(original, output == QStringLiteral("copy"));
        if (!artifact) {
            reply({}, QStringLiteral("not_ready"));
            return;
        }
        s.exports[r.connectionId].append(artifact);
        s.cancellations.insert(QString::number(r.connectionId) + u':' + r.requestId,
                               [guard, artifact, connection = r.connectionId, reply] {
                                   artifact->cancel();
                                   if (guard)
                                       guard->m_impl->exports[connection].removeAll(artifact);
                                   reply({}, QStringLiteral("cancelled"));
                               });
        const auto complete = [guard, artifact, connection = r.connectionId,
                               reply](QJsonObject result, QString error) {
            if (!guard)
                return;
            guard->m_impl->exports[connection].removeAll(artifact);
            reply(std::move(result), std::move(error));
        };
        if (output == QStringLiteral("copy")) {
            if (!artifact->requestClipboard(
                    this, [guard, complete, artifact, reply, requestId = r.requestId,
                           connection = r.connectionId](ScreenshotExportClipboardResult result) {
                        if (!guard)
                            return;
                        if (!result.succeeded()) {
                            complete({}, QStringLiteral("export_failed"));
                            return;
                        }
                        auto commit = ScreenshotClipboardService::commit(
                            QApplication::clipboard(), guard, std::move(result.payload),
                            [complete](ScreenshotClipboardCommitResult committed) {
                                complete({{QStringLiteral("copied"), committed.succeeded()}},
                                         committed.succeeded()
                                             ? QString()
                                             : QStringLiteral("clipboard_failed"));
                            });
                        if (!commit.isValid())
                            complete({}, QStringLiteral("clipboard_failed"));
                        else {
                            guard->m_impl->cancellations.insert(
                                QString::number(connection) + u':' + requestId,
                                [guard, artifact, commit, connection, reply] {
                                    artifact->cancel();
                                    commit.cancel();
                                    if (guard)
                                        guard->m_impl->exports[connection].removeAll(artifact);
                                    reply({}, QStringLiteral("cancelled"));
                                });
                            auto& commits = guard->m_impl->clipboardCommits[connection];
                            commits.append(std::move(commit));
                            while (commits.size() > 32)
                                commits.removeFirst();
                        }
                    }))
                complete({}, QStringLiteral("queue_full"));
        } else {
            const auto path = p.value(QStringLiteral("path")).toString();
            const auto format = ScreenshotImageFileService::formatForPath(path);
            if (!QFileInfo(path).isAbsolute() || QFileInfo::exists(path) || !format) {
                complete({}, QStringLiteral("invalid_path"));
                return;
            }
            // QTemporaryFile retains a native handle even after close(), which
            // prevents the encoder's QSaveFile from atomically replacing it on
            // Windows. A private staging directory needs no open file handle.
            auto staged = std::make_shared<QTemporaryDir>(
                QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral(".snow-mcp-XXXXXX")));
            if (!staged->isValid()) {
                complete({}, QStringLiteral("invalid_path"));
                return;
            }
            const auto stagingPath = staged->filePath(
                QStringLiteral("image.") + ScreenshotImageFileService::extension(*format));
            if (!artifact->requestSaveToPath(
                    this, stagingPath, *format, {},
                    [complete, staged, stagingPath, path](ScreenshotExportTaskResult result) {
                        // QFile::rename never overwrites an existing destination,
                        // including one created while the encoder was running.
                        const bool published =
                            result.succeeded() && QFile::rename(stagingPath, path);
                        complete({{QStringLiteral("path"), published ? path : QString()}},
                                 published ? QString() : QStringLiteral("export_failed"));
                    }))
                complete({}, QStringLiteral("queue_full"));
        }
        return;
    }
    reply({}, QStringLiteral("method_not_found"));
}

bool McpMediaService::cancelRequest(quint64 connection, const QString& requestId) {
    auto cancel = m_impl->cancellations.take(QString::number(connection) + u':' + requestId);
    if (!cancel)
        return false;
    cancel();
    return true;
}

void McpMediaService::disconnected(quint64 connection) {
    auto& s = *m_impl;
    if (s.recordingArtifacts.remove(connection))
        ++s.recordingArtifactRevision;
    const auto requestPrefix = QString::number(connection) + u':';
    for (const auto& key : s.cancellations.keys())
        if (key.startsWith(requestPrefix))
            cancelRequest(connection, key.mid(requestPrefix.size()));
    for (auto it = s.pending.begin(); it != s.pending.end();) {
        if (it->owner == connection) {
            it->job.cancel();
            if (!it->pinId.isEmpty())
                s.controller.destroyPinnedRecords({it->pinId});
            it = s.pending.erase(it);
        } else
            ++it;
    }
    for (const auto& id : s.recognitionOwners.take(connection))
        if (auto* window = s.groups.liveWindow(id))
            window->cancelAutomationRecognition();
    for (const auto& artifact : s.exports.take(connection))
        artifact->cancel();
    for (const auto& commit : s.clipboardCommits.take(connection))
        commit.cancel();
    if (s.recordingOwner == connection && s.recording) {
        s.recording->detachAutomation();
        s.recordingOwner = 0;
    }
    const auto prefix = QString::number(connection) + u':';
    for (auto it = s.replay.begin(); it != s.replay.end();) {
        if (it.key().startsWith(prefix)) {
            s.replayBytes -= it->bytes;
            it = s.replay.erase(it);
        } else
            ++it;
    }
    s.replayOrder.removeIf([&](const QString& key) { return key.startsWith(prefix); });
}
void McpMediaService::shutdown() {
    if (!m_impl || m_impl->stopped)
        return;
    auto& s = *m_impl;
    s.stopped = true;
    auto cancellations = std::move(s.cancellations);
    s.cancellations.clear();
    for (const auto& cancel : cancellations)
        cancel();
    for (const auto& job : s.pending) {
        job.job.cancel();
        if (!job.pinId.isEmpty())
            s.controller.destroyPinnedRecords({job.pinId});
    }
    s.pending.clear();
    for (const auto& list : s.exports)
        for (const auto& artifact : list)
            artifact->cancel();
    s.exports.clear();
    for (const auto& commits : s.clipboardCommits)
        for (const auto& commit : commits)
            commit.cancel();
    s.clipboardCommits.clear();
    for (const auto& ids : s.recognitionOwners)
        for (const auto& id : ids)
            if (auto* window = s.groups.liveWindow(id))
                window->cancelAutomationRecognition();
    s.recognitionOwners.clear();
    if (s.recordingOwner && s.recording)
        s.recording->detachAutomation();
}
} // namespace snow_shot::app::mcp
