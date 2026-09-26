#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <optional>

namespace snow_shot::app::mcp {
// All registry calls belong to the application thread. Worker completions must be queued.
class McpJobRegistry final : public QObject {
    Q_OBJECT
  public:
    explicit McpJobRegistry(QObject* parent = nullptr, std::function<qint64()> clock = {});
    QString start(quint64 owner, const QString& kind, std::function<void()> cancel = {},
                  QJsonObject metadata = {});
    bool complete(const QString& id, QJsonObject result);
    bool fail(const QString& id, const QString& code);
    bool progress(const QString& id, QJsonObject progress);
    bool setCancellation(const QString& id, std::function<void()> cancel);
    using ArtifactPublisher = std::function<QJsonObject(quint64, QByteArray, const QString&)>;
    void setArtifactPublisher(ArtifactPublisher publisher);
    bool retainInput(const QString& id, QJsonObject input);
    [[nodiscard]] std::optional<QJsonObject> input(quint64 owner, const QString& id) const;
    [[nodiscard]] std::optional<QJsonObject> get(quint64 owner, const QString& id) const;
    [[nodiscard]] QJsonArray list(quint64 owner) const;
    bool cancel(quint64 owner, const QString& id);
    // Retire an internal, unpublished reservation without invoking its cancellation callback.
    bool discard(quint64 owner, const QString& id);
    void disconnected(quint64 owner);
    void shutdown();
  signals:
    void changed(quint64 owner, const QString& id);

  private:
    struct Job {
        quint64 owner = 0;
        QJsonObject value;
        std::function<void()> cancellation;
        qsizetype bytes = 0;
        qint64 expires = 0;
        QJsonObject input;
        qsizetype inputBytes = 0;
    };
    bool finish(const QString& id, const QString& status, QJsonObject result);
    void trim();
    QHash<QString, Job> m_jobs;
    qsizetype m_bytes = 0;
    ArtifactPublisher m_artifactPublisher;
    std::function<qint64()> m_clock;
};
} // namespace snow_shot::app::mcp
