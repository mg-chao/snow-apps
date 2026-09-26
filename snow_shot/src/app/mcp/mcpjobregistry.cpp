#include "snow_shot/app/mcp/mcpjobregistry.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <utility>

namespace snow_shot::app::mcp {
McpJobRegistry::McpJobRegistry(QObject* parent, std::function<qint64()> clock)
    : QObject(parent),
      m_clock(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); }) {
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &McpJobRegistry::trim);
    timer->start(1000);
}
QString McpJobRegistry::start(quint64 owner, const QString& kind, std::function<void()> cancel,
                              QJsonObject metadata) {
    Q_ASSERT(QThread::currentThread() == thread());
    trim();
    int running = 0;
    int owned = 0;
    for (const auto& job : std::as_const(m_jobs)) {
        if (job.value.value(QStringLiteral("status")) != QStringLiteral("running"))
            continue;
        ++running;
        owned += job.owner == owner ? 1 : 0;
    }
    if (owner == 0 || kind.isEmpty() || running >= 32 || owned >= 8 || m_jobs.size() >= 64 ||
        QJsonDocument(metadata).toJson(QJsonDocument::Compact).size() > 65536)
        return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    metadata.insert(QStringLiteral("job_id"), id);
    metadata.insert(QStringLiteral("kind"), kind);
    metadata.insert(QStringLiteral("status"), QStringLiteral("running"));
    metadata.insert(QStringLiteral("cancelable"), bool(cancel));
    metadata.insert(QStringLiteral("ttl_ms"), 900000);
    metadata.insert(QStringLiteral("poll_interval_ms"), 250);
    metadata.insert(QStringLiteral("created_at"),
                    QDateTime::fromMSecsSinceEpoch(m_clock()).toUTC().toString(Qt::ISODateWithMs));
    metadata.insert(QStringLiteral("updated_at"), metadata.value(QStringLiteral("created_at")));
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact).size();
    if (m_bytes + bytes > 64 * 1024 * 1024 - 65536)
        return {};
    m_bytes += bytes;
    m_jobs.insert(id,
                  Job{owner, std::move(metadata), std::move(cancel), bytes, m_clock() + 900000});
    emit changed(owner, id);
    return id;
}
bool McpJobRegistry::finish(const QString& id, const QString& status, QJsonObject result) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->expires <= m_clock() ||
        found->value.value(QStringLiteral("status")) != QStringLiteral("running"))
        return false;
    const auto resultBytes = QJsonDocument(result).toJson(QJsonDocument::Compact).size();
    if (resultBytes > 8 * 1024 * 1024)
        return fail(id, QStringLiteral("output_too_large"));
    // Retain the terminal record until its advertised deadline, even when result memory is full.
    if (status == QStringLiteral("completed") &&
        m_bytes + resultBytes + 512 > 64 * 1024 * 1024 - 65536)
        return fail(id, QStringLiteral("resource_limit"));
    found->value.insert(QStringLiteral("status"), status);
    found->value.insert(QStringLiteral("cancelable"), false);
    found->value.insert(QStringLiteral("result"), std::move(result));
    found->value.insert(
        QStringLiteral("finished_at"),
        QDateTime::fromMSecsSinceEpoch(m_clock()).toUTC().toString(Qt::ISODateWithMs));
    found->value.insert(QStringLiteral("updated_at"),
                        found->value.value(QStringLiteral("finished_at")));
    found->value.remove(QStringLiteral("progress"));
    found->cancellation = {};
    m_bytes -= found->bytes;
    found->bytes =
        QJsonDocument(found->value).toJson(QJsonDocument::Compact).size() + found->inputBytes;
    m_bytes += found->bytes;
    const auto owner = found->owner;
    emit changed(owner, id);
    return true;
}
bool McpJobRegistry::complete(const QString& id, QJsonObject result) {
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (bytes.size() > 1024 * 1024 && m_artifactPublisher) {
        const auto job = m_jobs.constFind(id);
        if (job == m_jobs.cend() || job->expires <= m_clock() ||
            job->value.value(QStringLiteral("status")) != QStringLiteral("running"))
            return false;
        const auto artifact =
            m_artifactPublisher(job->owner, bytes, QStringLiteral("application/json"));
        if (artifact.isEmpty())
            return fail(id, QStringLiteral("resource_limit"));
        result = {{QStringLiteral("artifact"), artifact},
                  {QStringLiteral("format"), QStringLiteral("json")}};
    }
    return finish(id, QStringLiteral("completed"), std::move(result));
}
void McpJobRegistry::setArtifactPublisher(ArtifactPublisher publisher) {
    m_artifactPublisher = std::move(publisher);
}
bool McpJobRegistry::retainInput(const QString& id, QJsonObject inputValue) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->expires <= m_clock())
        return false;
    const auto bytes = QJsonDocument(inputValue).toJson(QJsonDocument::Compact).size();
    if (bytes > 1024 * 1024 || m_bytes - found->inputBytes + bytes > 64 * 1024 * 1024 - 65536)
        return false;
    m_bytes += bytes - found->inputBytes;
    found->bytes += bytes - found->inputBytes;
    found->inputBytes = bytes;
    found->input = std::move(inputValue);
    return true;
}
std::optional<QJsonObject> McpJobRegistry::input(quint64 owner, const QString& id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto found = m_jobs.constFind(id);
    if (found == m_jobs.cend() || found->owner != owner || found->expires <= m_clock() ||
        found->inputBytes == 0)
        return {};
    return found->input;
}
bool McpJobRegistry::fail(const QString& id, const QString& code) {
    return finish(id, QStringLiteral("failed"),
                  {{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}}}});
}
bool McpJobRegistry::progress(const QString& id, QJsonObject progressValue) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->expires <= m_clock() ||
        found->value.value(QStringLiteral("status")) != QStringLiteral("running") ||
        QJsonDocument(progressValue).toJson(QJsonDocument::Compact).size() > 16384)
        return false;
    auto value = found->value;
    value.insert(QStringLiteral("progress"), std::move(progressValue));
    value.insert(QStringLiteral("updated_at"),
                 QDateTime::fromMSecsSinceEpoch(m_clock()).toUTC().toString(Qt::ISODateWithMs));
    const auto bytes =
        QJsonDocument(value).toJson(QJsonDocument::Compact).size() + found->inputBytes;
    if (m_bytes - found->bytes + bytes > 64 * 1024 * 1024 - 65536)
        return false;
    m_bytes += bytes - found->bytes;
    found->bytes = bytes;
    found->value = std::move(value);
    emit changed(found->owner, id);
    return true;
}
bool McpJobRegistry::setCancellation(const QString& id, std::function<void()> cancel) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->expires <= m_clock() ||
        found->value.value(QStringLiteral("status")) != QStringLiteral("running"))
        return false;
    found->cancellation = std::move(cancel);
    found->value.insert(QStringLiteral("cancelable"), bool(found->cancellation));
    found->value.insert(
        QStringLiteral("updated_at"),
        QDateTime::fromMSecsSinceEpoch(m_clock()).toUTC().toString(Qt::ISODateWithMs));
    const auto bytes =
        QJsonDocument(found->value).toJson(QJsonDocument::Compact).size() + found->inputBytes;
    m_bytes += bytes - found->bytes;
    found->bytes = bytes;
    emit changed(found->owner, id);
    return true;
}
std::optional<QJsonObject> McpJobRegistry::get(quint64 owner, const QString& id) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto found = m_jobs.constFind(id);
    if (found == m_jobs.cend() || found->owner != owner || found->expires <= m_clock())
        return std::nullopt;
    return found->value;
}
QJsonArray McpJobRegistry::list(quint64 owner) const {
    Q_ASSERT(QThread::currentThread() == thread());
    QJsonArray result;
    for (const auto& job : m_jobs) {
        if (job.owner != owner || job.expires <= m_clock())
            continue;
        auto value = job.value;
        value.remove(QStringLiteral("result"));
        result.append(value);
    }
    return result;
}
bool McpJobRegistry::cancel(quint64 owner, const QString& id) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->owner != owner || found->expires <= m_clock())
        return false;
    if (found->value.value(QStringLiteral("status")) != QStringLiteral("running"))
        return true;
    if (!found->cancellation)
        return false;
    auto callback = std::move(found->cancellation);
    // Retire the job first: cancellation may synchronously deliver a late completion.
    finish(id, QStringLiteral("canceled"), {});
    callback();
    return true;
}
void McpJobRegistry::trim() {
    const qint64 now = m_clock();
    QVector<std::function<void()>> callbacks;
    QVector<QPair<quint64, QString>> removed;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->expires > now) {
            ++it;
            continue;
        }
        if (it->cancellation)
            callbacks.append(std::move(it->cancellation));
        m_bytes -= it->bytes;
        removed.append({it->owner, it.key()});
        it = m_jobs.erase(it);
    }
    for (const auto& callback : callbacks)
        callback();
    for (const auto& item : removed)
        emit changed(item.first, item.second);
}
bool McpJobRegistry::discard(quint64 owner, const QString& id) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->owner != owner)
        return false;
    m_bytes -= found->bytes;
    m_jobs.erase(found);
    emit changed(owner, id);
    return true;
}
void McpJobRegistry::disconnected(quint64 owner) {
    Q_ASSERT(QThread::currentThread() == thread());
    QVector<std::function<void()>> callbacks;
    QStringList removed;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->owner != owner) {
            ++it;
            continue;
        }
        if (it->cancellation)
            callbacks.append(std::move(it->cancellation));
        m_bytes -= it->bytes;
        removed.append(it.key());
        it = m_jobs.erase(it);
    }
    for (const auto& callback : callbacks)
        callback();
    for (const auto& id : removed)
        emit changed(owner, id);
}
void McpJobRegistry::shutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    while (!m_jobs.isEmpty())
        disconnected(m_jobs.cbegin()->owner);
}
} // namespace snow_shot::app::mcp
