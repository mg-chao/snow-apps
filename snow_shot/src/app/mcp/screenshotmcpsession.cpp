#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QUuid>
#include <utility>
#include <cmath>

namespace snow_shot::app::mcp {
namespace {
QString cacheKey(const ScreenshotMcpRequest& r) {
    return QString::number(r.connectionId) + u':' + r.idempotencyKey;
}
QByteArray fingerprint(const ScreenshotMcpRequest& r) {
    return QCryptographicHash::hash(
        QJsonDocument(QJsonObject{{QStringLiteral("method"), r.method},
                                  {QStringLiteral("session"), r.sessionId},
                                  {QStringLiteral("expected_revision"),
                                   static_cast<qint64>(r.expectedRevision.value_or(0))},
                                  {QStringLiteral("params"), r.params}})
            .toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
}
bool readOnly(const QString& method) {
    return method == QStringLiteral("snow_shot_status") ||
           method == QStringLiteral("screenshot_state") ||
           method == QStringLiteral("screenshot_operation") ||
           method == QStringLiteral("screenshot_render") ||
           method == QStringLiteral("screenshot_cancel");
}
const QStringList tools = {QStringLiteral("snow_shot_status"),
                           QStringLiteral("screenshot_begin"),
                           QStringLiteral("screenshot_state"),
                           QStringLiteral("screenshot_set_selection"),
                           QStringLiteral("screenshot_set_tool"),
                           QStringLiteral("screenshot_apply_annotations"),
                           QStringLiteral("screenshot_undo"),
                           QStringLiteral("screenshot_redo"),
                           QStringLiteral("screenshot_render"),
                           QStringLiteral("screenshot_save"),
                           QStringLiteral("screenshot_copy"),
                           QStringLiteral("screenshot_pin"),
                           QStringLiteral("screenshot_finish"),
                           QStringLiteral("screenshot_cancel"),
                           QStringLiteral("screenshot_direct_capture"),
                           QStringLiteral("screenshot_set_selection_style"),
                           QStringLiteral("screenshot_set_tool_style"),
                           QStringLiteral("screenshot_edit_elements"),
                           QStringLiteral("screenshot_recapture"),
                           QStringLiteral("screenshot_scrolling"),
                           QStringLiteral("screenshot_scroll_once"),
                           QStringLiteral("screenshot_recognize"),
                           QStringLiteral("screenshot_translate"),
                           QStringLiteral("screenshot_auto_filter"),
                           QStringLiteral("screenshot_operation"),
                           QStringLiteral("screenshot_edit_recognition"),
                           QStringLiteral("screenshot_export_recognition"),
                           QStringLiteral("screenshot_draw_template")};
} // namespace
ScreenshotMcpSession::ScreenshotMcpSession(Ports ports, QObject* parent)
    : QObject(parent), m_ports(std::move(ports)) {}
QJsonObject ScreenshotMcpSession::state() const {
    auto result = m_ports.state();
    result.insert(QStringLiteral("session_id"), m_session);
    result.insert(QStringLiteral("revision"), static_cast<qint64>(m_revision));
    result.insert(QStringLiteral("active"), !m_session.isEmpty());
    result.insert(QStringLiteral("pending_operation"),
                  m_pending ? m_pending->request.method : QString());
    result.insert(QStringLiteral("ready"), m_ready);
    QJsonArray operations;
    for (const auto& id : m_operationOrder) {
        auto summary = m_operations.value(id);
        summary.remove(QStringLiteral("result"));
        operations.append(summary);
    }
    result.insert(QStringLiteral("operations"), operations);
    return result;
}
ScreenshotMcpResponse ScreenshotMcpSession::failure(const ScreenshotMcpRequest& request,
                                                    const QString& code,
                                                    const QString& field) const {
    ScreenshotMcpResponse r;
    r.requestId = request.requestId;
    r.sessionId = m_session;
    r.revision = m_revision;
    r.errorCode = code;
    r.errorMessage = tr("MCP request failed (%1).").arg(code);
    r.errorDetails = {{QStringLiteral("state"), state()}};
    if (!field.isEmpty())
        r.errorDetails.insert(QStringLiteral("field"), field);
    return r;
}
void ScreenshotMcpSession::observe() {
    if (m_session.isEmpty() || m_releasing)
        return;
    const auto now = m_ports.state();
    if (now == m_observed)
        return;
    m_observed = now;
    ++m_revision;
    // An in-flight export owns its immutable snapshot. Retire only the retained cache;
    // cancellation is a separate explicit action.
    if (!m_pending)
        m_artifact.reset();
}
void ScreenshotMcpSession::startPending(const ScreenshotMcpRequest& r,
                                        ScreenshotMcpServer::Completion done) {
    Pending p;
    p.request = r;
    p.completion = std::move(done);
    p.generation = ++m_generation;
    p.sourceRevision = m_revision;
    p.timer.start();
    m_pending = std::move(p);
    ++m_revision;
    const auto generation = m_generation;
    QTimer::singleShot(60000, this, [this, generation] {
        if (!current(generation))
            return;
        const bool capture = m_pending->request.method == QStringLiteral("screenshot_begin");
        cancelPending(QStringLiteral("timeout"));
        if (capture)
            release(true);
    });
}
bool ScreenshotMcpSession::current(quint64 generation) const {
    return m_pending && m_pending->generation == generation;
}
void ScreenshotMcpSession::complete(ScreenshotMcpResponse response) {
    if (!m_pending)
        return;
    auto pending = std::move(*m_pending);
    m_pending.reset();
    ++m_revision;
    response.requestId = pending.request.requestId;
    response.sessionId = m_session;
    response.revision = m_revision;
    response.result.insert(QStringLiteral("session_id"), m_session);
    response.result.insert(QStringLiteral("revision"), static_cast<qint64>(m_revision));
    response.result.insert(QStringLiteral("source_revision"),
                           static_cast<qint64>(pending.sourceRevision));
    pending.timings.insert(QStringLiteral("total"), pending.timer.elapsed());
    if (pending.request.method == QStringLiteral("screenshot_begin"))
        pending.timings.insert(QStringLiteral("capture_and_reconciliation"),
                               pending.timer.elapsed());
    response.result.insert(QStringLiteral("timings_ms"), pending.timings);
    if (!response.ok)
        response.errorDetails.insert(QStringLiteral("state"), state());
    response.result.insert(QStringLiteral("pending_operation"), QString());
    cache(pending.request, response);
    pending.completion(std::move(response));
    const auto now = m_ports.state();
    if (now != m_artifactState)
        m_artifact.reset();
    m_observed = now;
}
void ScreenshotMcpSession::failPending(const QString& code, const QString& field) {
    if (m_pending) {
        const bool direct =
            m_pending->request.method == QStringLiteral("screenshot_direct_capture");
        complete(failure(m_pending->request, code, field));
        if (direct)
            release(false);
    }
}
void ScreenshotMcpSession::cancelPending(const QString& code) {
    if (!m_pending)
        return;
    // Retire callbacks before stopping work: controller cancellation may complete synchronously.
    m_pending->generation = ++m_generation;
    if (m_artifact)
        m_artifact->cancel();
    m_clipboard.cancel();
    m_metadataJob.cancel();
    if (m_ports.cancelCommand)
        m_ports.cancelCommand();
    failPending(code);
}
void ScreenshotMcpSession::cache(const ScreenshotMcpRequest& request,
                                 const ScreenshotMcpResponse& response) {
    if (readOnly(request.method) || request.idempotencyKey.isEmpty())
        return;
    const auto key = cacheKey(request);
    if (m_cache.contains(key))
        return;
    // A bounded replay window. Revisions still protect mutations after a cache eviction.
    while (!m_cacheOrder.isEmpty() &&
           (m_cacheOrder.size() >= 64 ||
            m_cacheBytes + response.attachment.size() > 64 * 1024 * 1024)) {
        auto prior = m_cache.take(m_cacheOrder.dequeue());
        m_cacheBytes -= prior.response.attachment.size();
    }
    m_cacheBytes += response.attachment.size();
    m_cache.insert(key, {fingerprint(request), response});
    m_cacheOrder.enqueue(key);
}
void ScreenshotMcpSession::capturePresented() {
    if (m_session.isEmpty())
        return;
    m_ready = true;
    observe();
    if (m_pending && m_pending->request.method == QStringLiteral("screenshot_begin")) {
        ScreenshotMcpResponse response;
        response.ok = true;
        response.result = state();
        complete(response);
    }
}
void ScreenshotMcpSession::captureTerminated() {
    if (m_session.isEmpty() || m_releasing)
        return;
    failPending(m_ready ? QStringLiteral("canceled") : QStringLiteral("capture_unavailable"));
    release(false);
}
void ScreenshotMcpSession::release(bool cancel) {
    cancelOperation();
    if (m_ports.cancelCommand)
        m_ports.cancelCommand();
    if (m_ports.detached)
        m_ports.detached();
    m_operations.clear();
    m_operationOrder.clear();
    if (m_artifact)
        m_artifact->cancel();
    m_artifact.reset();
    m_clipboard.cancel();
    m_metadataJob.cancel();
    m_releasing = true;
    if (cancel && !m_session.isEmpty())
        m_ports.cancel();
    m_releasing = false;
    ++m_revision;
    m_owner = 0;
    m_session.clear();
    m_ready = false;
    m_silent = false;
    m_observed = {};
    m_artifactState = {};
    m_captureMetadata = {};
    m_pngMetadata = {};
}
void ScreenshotMcpSession::disconnected(quint64 connection) {
    const QString prefix = QString::number(connection) + u':';
    for (auto it = m_cacheOrder.begin(); it != m_cacheOrder.end();) {
        if (it->startsWith(prefix)) {
            m_cacheBytes -= m_cache.take(*it).response.attachment.size();
            it = m_cacheOrder.erase(it);
        } else
            ++it;
    }
    if (connection != m_owner)
        return;
    failPending(QStringLiteral("canceled"));
    release(m_silent);
}
void ScreenshotMcpSession::shutdown() {
    failPending(QStringLiteral("disabled"));
    release(m_silent);
    m_cache.clear();
    m_cacheOrder.clear();
    m_cacheBytes = 0;
}
void ScreenshotMcpSession::request(const ScreenshotMcpRequest& r,
                                   ScreenshotMcpServer::Completion done) {
    Q_ASSERT(QThread::currentThread() == thread());
    observe();
    const auto reject = [&](const QString& code, const QString& field = QString()) {
        done(failure(r, code, field));
    };
    if (!tools.contains(r.method)) {
        reject(QStringLiteral("method_not_found"));
        return;
    }
    if (!readOnly(r.method)) {
        if (r.idempotencyKey.isEmpty() || r.idempotencyKey.size() > 128) {
            reject(QStringLiteral("invalid_parameters"), QStringLiteral("idempotency_key"));
            return;
        }
        const auto found = m_cache.constFind(cacheKey(r));
        if (found != m_cache.cend()) {
            if (found->fingerprint != fingerprint(r)) {
                reject(QStringLiteral("idempotency_conflict"));
                return;
            }
            auto response = found->response;
            response.requestId = r.requestId;
            done(std::move(response));
            return;
        }
    }
    if (r.method == QStringLiteral("snow_shot_status")) {
        ScreenshotMcpResponse response;
        response.ok = true;
        response.result = {
            {QStringLiteral("reachable"), true},
            {QStringLiteral("mcp_enabled"), true},
            {QStringLiteral("application_version"), QCoreApplication::applicationVersion()},
            {QStringLiteral("protocol_version"), QStringLiteral("snow-shot-mcp/1")},
            {QStringLiteral("active_session"), !m_session.isEmpty()},
            {QStringLiteral("ownership"),
             m_session.isEmpty()
                 ? QStringLiteral("none")
                 : (r.connectionId == m_owner ? QStringLiteral("self") : QStringLiteral("other"))},
            {QStringLiteral("capabilities"), QJsonArray::fromStringList(tools)}};
        done(response);
        return;
    }
    if (r.method == QStringLiteral("screenshot_cancel")) {
        if (m_session.isEmpty()) {
            ScreenshotMcpResponse response;
            response.ok = true;
            response.result = {{QStringLiteral("canceled"), false}};
            done(response);
            return;
        }
        if (r.connectionId != m_owner || (!r.sessionId.isEmpty() && r.sessionId != m_session)) {
            reject(QStringLiteral("session_not_found"));
            return;
        }
        const QString operationId = r.params.value(QStringLiteral("operation_id")).toString();
        if (!operationId.isEmpty()) {
            if (!m_operations.contains(operationId)) {
                reject(QStringLiteral("operation_not_found"));
                return;
            }
            if (operationId == m_activeOperation)
                cancelOperation();
            ScreenshotMcpResponse response;
            response.ok = true;
            response.sessionId = m_session;
            response.revision = m_revision;
            response.result = m_operations.value(operationId);
            done(response);
            return;
        }
        const QString requestId = r.params.value(QStringLiteral("request_id")).toString();
        if (!requestId.isEmpty() && (!m_pending || m_pending->request.requestId != requestId)) {
            reject(QStringLiteral("request_not_found"));
            return;
        }
        const bool capture =
            m_pending && m_pending->request.method == QStringLiteral("screenshot_begin");
        cancelPending(QStringLiteral("canceled"));
        m_artifact.reset();
        if (requestId.isEmpty() || capture)
            release(true);
        ScreenshotMcpResponse response;
        response.ok = true;
        response.result = state();
        response.result.insert(QStringLiteral("canceled"), true);
        done(response);
        return;
    }
    if (r.method == QStringLiteral("screenshot_begin") ||
        r.method == QStringLiteral("screenshot_direct_capture")) {
        if (!m_session.isEmpty() ||
            m_ports.state().value(QStringLiteral("capture_phase")).toString() !=
                QStringLiteral("idle")) {
            reject(QStringLiteral("busy"));
            return;
        }
        m_owner = r.connectionId;
        m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_silent =
            r.params.value(QStringLiteral("presentation")).toString() == QStringLiteral("silent") ||
            r.method == QStringLiteral("screenshot_direct_capture");
        m_revision = 1;
        m_ready = false;
        m_observed = m_ports.state();
        startPending(r, std::move(done));
        if (r.method == QStringLiteral("screenshot_begin")) {
            QString error;
            if (!m_ports.begin(r.params, &error)) {
                const auto code = (error == QStringLiteral("busy") ||
                                   error == QStringLiteral("permission_required") ||
                                   error == QStringLiteral("capture_unavailable"))
                                      ? error
                                      : QStringLiteral("invalid_parameters");
                failPending(code, error);
                release(false);
            }
        } else {
            const auto generation = m_generation;
            QPointer<ScreenshotMcpSession> guard(this);
            if (!m_ports.direct(r.params, [this, guard, generation,
                                           r](QImage image, QJsonObject metadata, QString error) {
                    if (!guard || !current(generation))
                        return;
                    if (image.isNull() || !error.isEmpty()) {
                        failPending(QStringLiteral("capture_unavailable"));
                        release(false);
                        return;
                    }
                    m_artifact = std::make_shared<ScreenshotExportArtifact>(
                        ScreenshotExportSource::fromImage(std::move(image)));
                    m_captureMetadata = metadata;
                    m_artifactState = m_ports.state();
                    m_artifactScale = r.params.value(QStringLiteral("scale")).toDouble(1);
                    m_ready = true;
                    output(r, true);
                })) {
                failPending(QStringLiteral("busy"));
                release(false);
            }
        }
        return;
    }
    if (m_session.isEmpty() || r.sessionId != m_session || r.connectionId != m_owner) {
        reject(QStringLiteral("session_not_found"));
        return;
    }
    if (r.method == QStringLiteral("screenshot_state")) {
        ScreenshotMcpResponse response;
        response.ok = true;
        response.sessionId = m_session;
        response.revision = m_revision;
        response.result = state();
        done(response);
        return;
    }
    if (r.method == QStringLiteral("screenshot_operation")) {
        const auto id = r.params.value(QStringLiteral("operation_id")).toString();
        if (!m_operations.contains(id)) {
            reject(QStringLiteral("operation_not_found"));
            return;
        }
        ScreenshotMcpResponse response;
        response.ok = true;
        response.sessionId = m_session;
        response.revision = m_revision;
        response.result = m_operations.value(id);
        done(response);
        return;
    }
    if (!r.expectedRevision) {
        reject(QStringLiteral("revision_required"));
        return;
    }
    if (*r.expectedRevision != m_revision) {
        reject(QStringLiteral("stale_revision"));
        return;
    }
    const bool invalidates = r.method == QStringLiteral("screenshot_set_selection") ||
                             r.method == QStringLiteral("screenshot_recapture") ||
                             r.method == QStringLiteral("screenshot_finish");
    if (!m_activeOperation.isEmpty() && !invalidates) {
        reject(QStringLiteral("busy"));
        return;
    }
    if (m_pending || !m_ready) {
        reject(QStringLiteral("busy"));
        return;
    }
    if (invalidates) {
        cancelOperation();
        m_operations.clear();
        m_operationOrder.clear();
    }
    const bool background = r.method == QStringLiteral("screenshot_recognize") ||
                            r.method == QStringLiteral("screenshot_translate") ||
                            r.method == QStringLiteral("screenshot_auto_filter");
    const bool extended =
        r.method == QStringLiteral("screenshot_set_selection_style") ||
        r.method == QStringLiteral("screenshot_set_tool_style") ||
        r.method == QStringLiteral("screenshot_edit_elements") ||
        r.method == QStringLiteral("screenshot_recapture") ||
        r.method == QStringLiteral("screenshot_scrolling") ||
        r.method == QStringLiteral("screenshot_scroll_once") ||
        r.method == QStringLiteral("screenshot_edit_recognition") ||
        r.method == QStringLiteral("screenshot_export_recognition") ||
        r.method == QStringLiteral("screenshot_draw_template") ||
        ((r.method == QStringLiteral("screenshot_undo") ||
          r.method == QStringLiteral("screenshot_redo")) &&
         r.params.value(QStringLiteral("target")).toString(QStringLiteral("canvas")) !=
             QStringLiteral("canvas"));
    if (background || extended) {
        if (!m_ports.command) {
            reject(QStringLiteral("unsupported"));
            return;
        }
        QPointer<ScreenshotMcpSession> guard(this);
        if (background) {
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto generation = ++m_operationGeneration;
            const auto sourceRevision = m_revision;
            m_activeOperation = id;
            m_operationOrder.enqueue(id);
            m_operations.insert(
                id, {{QStringLiteral("operation_id"), id},
                     {QStringLiteral("status"), QStringLiteral("running")},
                     {QStringLiteral("method"), r.method},
                     {QStringLiteral("source_revision"), static_cast<qint64>(sourceRevision)}});
            ++m_revision;
            m_ports.command(r.method, r.params,
                            [this, guard, generation, id](QJsonObject result, QString error) {
                                if (!guard || generation != m_operationGeneration ||
                                    m_activeOperation != id)
                                    return;
                                m_activeOperation.clear();
                                auto& operation = m_operations[id];
                                if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
                                    32 * 1024 * 1024) {
                                    result = {};
                                    error = QStringLiteral("output_too_large");
                                }
                                operation.insert(QStringLiteral("status"),
                                                 error.isEmpty() ? QStringLiteral("completed")
                                                                 : QStringLiteral("failed"));
                                operation.insert(QStringLiteral("result"), result);
                                if (!error.isEmpty())
                                    operation.insert(QStringLiteral("error"),
                                                     QJsonObject{{QStringLiteral("code"), error}});
                                observe();
                                ++m_revision;
                                trimOperations();
                            });
            ScreenshotMcpResponse response;
            response.ok = true;
            response.sessionId = m_session;
            response.revision = m_revision;
            response.result = m_operations.value(id);
            cache(r, response);
            done(response);
        } else {
            startPending(r, std::move(done));
            const auto generation = m_generation;
            m_ports.command(r.method, r.params,
                            [this, guard, generation](QJsonObject result, QString error) {
                                if (!guard || !current(generation))
                                    return;
                                ScreenshotMcpResponse response;
                                if (error.isEmpty()) {
                                    response.ok = true;
                                    response.result = state();
                                    for (auto it = result.begin(); it != result.end(); ++it)
                                        response.result.insert(it.key(), it.value());
                                } else
                                    response = failure(m_pending->request, error);
                                complete(std::move(response));
                            });
        }
        return;
    }
    if (r.method == QStringLiteral("screenshot_render") ||
        r.method == QStringLiteral("screenshot_save") ||
        r.method == QStringLiteral("screenshot_copy") ||
        r.method == QStringLiteral("screenshot_pin") ||
        (r.method == QStringLiteral("screenshot_finish") &&
         r.params.value(QStringLiteral("output")).toString() != QStringLiteral("none") &&
         r.params.contains(QStringLiteral("output")))) {
        startPending(r, std::move(done));
        output(r, r.method == QStringLiteral("screenshot_finish"));
        return;
    }
    QString error;
    QJsonObject annotation;
    bool ok = false;
    if (r.method == QStringLiteral("screenshot_set_selection"))
        ok = m_ports.selection(r.params, &error);
    else if (r.method == QStringLiteral("screenshot_set_tool"))
        ok = m_ports.tool(r.params.value(QStringLiteral("tool")).toString(), &error);
    else if (r.method == QStringLiteral("screenshot_apply_annotations")) {
        const QJsonObject batch{
            {QStringLiteral("version"), r.params.value(QStringLiteral("version")).toInt(1)},
            {QStringLiteral("label"),
             r.params.value(QStringLiteral("label")).toString(QStringLiteral("MCP annotation"))},
            {QStringLiteral("operations"), r.params.value(QStringLiteral("operations"))}};
        ok = m_ports.annotations(QJsonDocument(batch).toJson(QJsonDocument::Compact), &annotation,
                                 &error);
    } else if (r.method == QStringLiteral("screenshot_undo") ||
               r.method == QStringLiteral("screenshot_redo")) {
        m_ports.history(r.method == QStringLiteral("screenshot_redo"));
        ok = true;
    } else if (r.method == QStringLiteral("screenshot_finish")) {
        release(true);
        ok = true;
    }
    if (!ok) {
        reject(QStringLiteral("invalid_parameters"), error);
        return;
    }
    observe();
    ScreenshotMcpResponse response;
    response.ok = true;
    response.sessionId = m_session;
    response.revision = m_revision;
    response.result = state();
    if (!annotation.isEmpty())
        response.result.insert(QStringLiteral("transaction"), annotation);
    cache(r, response);
    done(response);
}
void ScreenshotMcpSession::cancelOperation() {
    ++m_operationGeneration;
    if (m_activeOperation.isEmpty())
        return;
    m_operations[m_activeOperation].insert(QStringLiteral("status"), QStringLiteral("canceled"));
    m_activeOperation.clear();
    if (m_ports.cancelCommand)
        m_ports.cancelCommand();
    ++m_revision;
    trimOperations();
}
void ScreenshotMcpSession::trimOperations() {
    qsizetype bytes = 0;
    for (const auto& operation : m_operations)
        bytes += QJsonDocument(operation).toJson(QJsonDocument::Compact).size();
    while (!m_operationOrder.isEmpty() &&
           (m_operationOrder.size() > 16 || bytes > 32 * 1024 * 1024)) {
        const auto previous = m_operations.take(m_operationOrder.dequeue());
        bytes -= QJsonDocument(previous).toJson(QJsonDocument::Compact).size();
    }
}
bool ScreenshotMcpSession::validateOutputPath(const QString& path, QString* canonical) {
    if (path.isEmpty() || path.contains(QChar::Null) || path.startsWith(QStringLiteral("\\\\")) ||
        path.startsWith(QStringLiteral("//")) || path.contains(QStringLiteral("://")))
        return false;
    const QFileInfo info(path);
    if (!info.isAbsolute() || info.isDir() || info.isSymLink() || info.fileName().isEmpty())
        return false;
#ifdef Q_OS_WIN
    if (!QRegularExpression(QStringLiteral("^[A-Za-z]:[/\\\\]")).match(path).hasMatch() ||
        path.mid(2).contains(u':') ||
        QRegularExpression(QStringLiteral("[<>\"|?*]")).match(path).hasMatch() ||
        path.endsWith(u'.') || path.endsWith(u' '))
        return false;
    const auto parts = QDir::fromNativeSeparators(path).split(u'/');
    const QRegularExpression device(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
                                    QRegularExpression::CaseInsensitiveOption);
    for (const auto& part : parts)
        if (device.match(part).hasMatch())
            return false;
#endif
    const QString parent = info.dir().canonicalPath();
    if (parent.isEmpty())
        return false;
    if (canonical)
        *canonical = QDir(parent).filePath(info.fileName());
    return true;
}
void ScreenshotMcpSession::output(const ScreenshotMcpRequest& r, bool finish) {
    const auto generation = m_generation;
    const double scale = r.params.value(QStringLiteral("scale")).toDouble(1);
    if (!std::isfinite(scale) || scale < 0.1 || scale > 4) {
        failPending(QStringLiteral("invalid_parameters"), QStringLiteral("scale"));
        return;
    }
    if (!m_artifact || m_artifact->isCancelled() || scale != m_artifactScale ||
        m_artifactState != m_ports.state()) {
        m_pngMetadata = {};
        m_artifact = m_ports.artifact(scale);
        m_artifactScale = scale;
        m_artifactState = m_ports.state();
    }
    if (!m_artifact) {
        failPending(QStringLiteral("output_failed"));
        return;
    }
    if (!m_pngMetadata.isEmpty()) {
        m_pending->timings.insert(QStringLiteral("cache_hit"), true);
        publishOutput(r, finish, generation, m_pngMetadata);
        return;
    }
    auto artifact = m_artifact;
    QElapsedTimer renderTimer;
    renderTimer.start();
    if (!artifact->requestImage(this, [this, generation, r, finish, scale,
                                       renderTimer](ScreenshotExportImageResult result) {
            if (!current(generation))
                return;
            m_pending->timings.insert(QStringLiteral("render"), renderTimer.elapsed());
            if (!result.succeeded()) {
                failPending(QStringLiteral("output_failed"));
                return;
            }
            encodeOutput(r, finish, generation, scale);
        }))
        failPending(QStringLiteral("output_failed"));
}
void ScreenshotMcpSession::encodeOutput(const ScreenshotMcpRequest& r, bool finish,
                                        quint64 generation, qreal scale) {
    auto artifact = m_artifact;
    QElapsedTimer encodeTimer;
    encodeTimer.start();
    if (!artifact->requestCanonicalPng(this, [this, generation, r, finish, artifact, scale,
                                              encodeTimer](ScreenshotExportEncodingResult encoded) {
            if (!current(generation))
                return;
            if (!encoded.succeeded()) {
                failPending(QStringLiteral("output_failed"));
                return;
            }
            m_pending->timings.insert(QStringLiteral("encode"), encodeTimer.elapsed());
            const auto png = encoded.image;
            auto metadata = std::make_shared<QJsonObject>();
            m_metadataJob = ScreenshotExportCoordinator::shared().submit(
                this, ScreenshotExportCoordinator::Priority::Foreground,
                [metadata, png, scale](const ScreenshotExportCancellation& cancellation) {
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, QStringLiteral("canceled"));
                    *metadata = {{QStringLiteral("width"), png.pixelSize().width()},
                                 {QStringLiteral("height"), png.pixelSize().height()},
                                 {QStringLiteral("scale"), scale},
                                 {QStringLiteral("format"), QStringLiteral("png")},
                                 {QStringLiteral("byte_count"), png.bytes().size()},
                                 {QStringLiteral("sha256"),
                                  QString::fromLatin1(QCryptographicHash::hash(
                                                          png.bytes(), QCryptographicHash::Sha256)
                                                          .toHex())}};
                    return ScreenshotExportTaskResult{};
                },
                [this, generation, r, finish, metadata](ScreenshotExportTaskResult result) {
                    if (!current(generation))
                        return;
                    if (!result.succeeded()) {
                        failPending(QStringLiteral("canceled"));
                        return;
                    }
                    metadata->insert(QStringLiteral("selection"),
                                     m_artifactState.value(QStringLiteral("selection")));
                    metadata->insert(QStringLiteral("canvas_bounds"),
                                     m_artifactState.value(QStringLiteral("canvas_bounds")));
                    m_pngMetadata = *metadata;
                    publishOutput(r, finish, generation, *metadata);
                });
            if (!m_metadataJob.isValid())
                failPending(QStringLiteral("queue_full"));
        }))
        failPending(QStringLiteral("output_failed"));
}
void ScreenshotMcpSession::publishOutput(const ScreenshotMcpRequest& r, bool finish,
                                         quint64 generation, const QJsonObject& metadata) {
    auto artifact = m_artifact;
    QString action = r.method;
    if (action == QStringLiteral("screenshot_finish") ||
        action == QStringLiteral("screenshot_direct_capture"))
        action = QStringLiteral("screenshot_") +
                 r.params.value(QStringLiteral("output")).toString(QStringLiteral("render"));
    const auto finishResponse = [this, generation, finish](ScreenshotMcpResponse response) {
        if (!current(generation))
            return;
        const bool successful = response.ok;
        if (!m_captureMetadata.isEmpty())
            response.result.insert(QStringLiteral("capture"), m_captureMetadata);
        if (finish && successful) {
            const auto finishedSession = m_session;
            release(true);
            response.result.insert(QStringLiteral("finished_session_id"), finishedSession);
            response.result.insert(QStringLiteral("finished"), true);
        }
        complete(std::move(response));
    };
    if (action == QStringLiteral("screenshot_render")) {
        if (!artifact->requestCanonicalPng(this, [this, generation, metadata, finishResponse](
                                                     ScreenshotExportEncodingResult png) {
                if (!current(generation))
                    return;
                if (!png.succeeded() || png.image.bytes().size() > 64 * 1024 * 1024 - 65536) {
                    failPending(QStringLiteral("output_too_large"));
                    return;
                }
                ScreenshotMcpResponse response;
                response.ok = true;
                response.result = metadata;
                response.attachment = png.image.bytes();
                response.attachmentMime = QStringLiteral("image/png");
                finishResponse(response);
            }))
            failPending(QStringLiteral("output_failed"));
    } else if (action == QStringLiteral("screenshot_copy")) {
        if (m_ports.copy) {
            if (!m_ports.copy(artifact, [this, generation, metadata, finishResponse](bool success) {
                    if (!current(generation))
                        return;
                    if (!success) {
                        failPending(QStringLiteral("clipboard_failed"));
                        return;
                    }
                    ScreenshotMcpResponse response;
                    response.ok = true;
                    response.result = metadata;
                    response.result.insert(QStringLiteral("copied"), true);
                    finishResponse(response);
                }))
                failPending(QStringLiteral("clipboard_failed"));
            return;
        }
        if (!artifact->requestClipboard(this, [this, generation, metadata, finishResponse](
                                                  ScreenshotExportClipboardResult payload) {
                if (!current(generation))
                    return;
                if (!payload.succeeded()) {
                    failPending(QStringLiteral("output_failed"));
                    return;
                }
                m_clipboard = ScreenshotClipboardService::commit(
                    QApplication::clipboard(), this, std::move(payload.payload),
                    [this, generation, metadata,
                     finishResponse](ScreenshotClipboardCommitResult commit) {
                        if (!current(generation))
                            return;
                        if (!commit.succeeded()) {
                            failPending(QStringLiteral("clipboard_failed"));
                            return;
                        }
                        ScreenshotMcpResponse response;
                        response.ok = true;
                        response.result = metadata;
                        response.result.insert(QStringLiteral("copied"), true);
                        finishResponse(response);
                    });
                if (!m_clipboard.isValid())
                    failPending(QStringLiteral("clipboard_failed"));
            }))
            failPending(QStringLiteral("output_failed"));
    } else if (action == QStringLiteral("screenshot_pin")) {
        if (!m_ports.pin(artifact, [this, generation, metadata, finishResponse](bool ok) {
                if (!current(generation))
                    return;
                if (!ok) {
                    failPending(QStringLiteral("pin_failed"));
                    return;
                }
                ScreenshotMcpResponse response;
                response.ok = true;
                response.result = metadata;
                response.result.insert(QStringLiteral("pinned"), true);
                finishResponse(response);
            }))
            failPending(QStringLiteral("pin_failed"));
    } else if (action == QStringLiteral("screenshot_save")) {
        const QString format =
            r.params.value(QStringLiteral("format")).toString(QStringLiteral("png"));
        if (!QStringList{QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("webp"),
                         QStringLiteral("avif"), QStringLiteral("pdf")}
                 .contains(format)) {
            failPending(QStringLiteral("invalid_parameters"), QStringLiteral("format"));
            return;
        }
        QString path = r.params.value(QStringLiteral("path")).toString();
        if (path.isEmpty() && r.params.value(QStringLiteral("automatic_path")).toBool()) {
            const auto dirs = ScreenshotImageFileService::automaticDirectories();
            if (!dirs.isEmpty())
                path =
                    QDir(dirs.first())
                        .filePath(ScreenshotImageFileService::suggestedBaseName() + u'.' + format);
        }
        QString canonical;
        if (!validateOutputPath(path, &canonical)) {
            failPending(QStringLiteral("invalid_parameters"), QStringLiteral("path"));
            return;
        }
        ScreenshotImageEncodingOptions encoding;
        encoding.quality = r.params.value(QStringLiteral("quality")).toInt(100);
        if (encoding.quality < 1 || encoding.quality > 100) {
            failPending(QStringLiteral("invalid_parameters"), QStringLiteral("quality"));
            return;
        }
        if (!artifact->requestSaveToPath(
                this, canonical, ScreenshotImageFileService::formatForKey(format), encoding,
                [this, generation, metadata, format,
                 finishResponse](ScreenshotExportTaskResult result) {
                    if (!current(generation))
                        return;
                    if (!result.succeeded()) {
                        failPending(QStringLiteral("output_failed"));
                        return;
                    }
                    auto saved = std::make_shared<QJsonObject>(metadata);
                    const QString savedPath = result.savedPath;
                    m_metadataJob = ScreenshotExportCoordinator::shared().submit(
                        this, ScreenshotExportCoordinator::Priority::Foreground,
                        [saved, savedPath,
                         format](const ScreenshotExportCancellation& cancellation) {
                            QFile file(savedPath);
                            if (!file.open(QIODevice::ReadOnly))
                                return ScreenshotExportTaskResult::failure(
                                    ScreenshotExportFailureStage::File,
                                    QStringLiteral("read_failed"));
                            QCryptographicHash hash(QCryptographicHash::Sha256);
                            while (!file.atEnd()) {
                                if (cancellation.isCancellationRequested())
                                    return ScreenshotExportTaskResult::failure(
                                        ScreenshotExportFailureStage::Cancelled,
                                        QStringLiteral("canceled"));
                                auto bytes = file.read(1024 * 1024);
                                if (bytes.isEmpty() && !file.atEnd())
                                    return ScreenshotExportTaskResult::failure(
                                        ScreenshotExportFailureStage::File,
                                        QStringLiteral("read_failed"));
                                hash.addData(bytes);
                            }
                            saved->insert(QStringLiteral("sha256"),
                                          QString::fromLatin1(hash.result().toHex()));
                            saved->insert(QStringLiteral("byte_count"), file.size());
                            saved->insert(QStringLiteral("path"), savedPath);
                            saved->insert(QStringLiteral("format"), format);
                            return ScreenshotExportTaskResult{};
                        },
                        [this, generation, saved,
                         finishResponse](ScreenshotExportTaskResult verified) {
                            if (!current(generation))
                                return;
                            if (!verified.succeeded()) {
                                failPending(QStringLiteral("output_failed"));
                                return;
                            }
                            ScreenshotMcpResponse response;
                            response.ok = true;
                            response.result = *saved;
                            finishResponse(response);
                        });
                    if (!m_metadataJob.isValid())
                        failPending(QStringLiteral("queue_full"));
                }))
            failPending(QStringLiteral("output_failed"));
    } else
        failPending(QStringLiteral("invalid_parameters"), QStringLiteral("output"));
}
} // namespace snow_shot::app::mcp
