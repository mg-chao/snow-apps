#pragma once
#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include <QElapsedTimer>
#include <QHash>
#include <QQueue>

namespace snow_shot::app::mcp {
class ScreenshotMcpSession final : public QObject {
    Q_OBJECT
  public:
    struct Ports {
        std::function<QJsonObject()> state;
        std::function<bool(const QJsonObject&, QString*)> begin;
        std::function<void()> cancel;
        std::function<bool(const QJsonObject&, QString*)> selection;
        std::function<bool(const QString&, QString*)> tool;
        std::function<bool(const QByteArray&, QJsonObject*, QString*)> annotations;
        std::function<void(bool)> history;
        using CommandCompletion = std::function<void(QJsonObject, QString)>;
        std::function<void(const QString&, const QJsonObject&, CommandCompletion)> command;
        std::function<void()> cancelCommand;
        std::function<void()> detached;
        std::function<std::shared_ptr<ScreenshotExportArtifact>(qreal)> artifact;
        // Optional adapter for deterministic clipboard publication in integration tests.
        // The application leaves this unset and uses ScreenshotClipboardService directly.
        std::function<bool(std::shared_ptr<ScreenshotExportArtifact>, std::function<void(bool)>)>
            copy;
        std::function<bool(std::shared_ptr<ScreenshotExportArtifact>, std::function<void(bool)>)>
            pin;
        using DirectCompletion = std::function<void(QImage, QJsonObject, QString)>;
        std::function<bool(const QJsonObject&, DirectCompletion)> direct;
    };
    explicit ScreenshotMcpSession(Ports ports, QObject* parent = nullptr);
    void request(const ScreenshotMcpRequest& request, ScreenshotMcpServer::Completion completion);
    void observe();
    void capturePresented();
    void captureTerminated();
    void disconnected(quint64 connectionId);
    void shutdown();
    [[nodiscard]] QJsonObject state() const;
    [[nodiscard]] static bool validateOutputPath(const QString& path, QString* canonical);

  private:
    struct Pending {
        ScreenshotMcpRequest request;
        ScreenshotMcpServer::Completion completion;
        quint64 generation = 0;
        quint64 sourceRevision = 0;
        QElapsedTimer timer;
        QJsonObject timings;
    };
    struct Cached {
        QByteArray fingerprint;
        ScreenshotMcpResponse response;
    };
    ScreenshotMcpResponse failure(const ScreenshotMcpRequest&, const QString& code,
                                  const QString& field = {}) const;
    void startPending(const ScreenshotMcpRequest&, ScreenshotMcpServer::Completion);
    void complete(ScreenshotMcpResponse response);
    void failPending(const QString& code, const QString& field = {});
    void cancelPending(const QString& code);
    void release(bool cancel);
    void output(const ScreenshotMcpRequest&, bool finish);
    void encodeOutput(const ScreenshotMcpRequest&, bool finish, quint64 generation, qreal scale);
    void publishOutput(const ScreenshotMcpRequest&, bool finish, quint64 generation,
                       const QJsonObject& metadata);
    bool current(quint64 generation) const;
    void cache(const ScreenshotMcpRequest&, const ScreenshotMcpResponse&);
    void cancelOperation();
    void trimOperations();
    Ports m_ports;
    QString m_session;
    quint64 m_owner = 0;
    quint64 m_revision = 0;
    quint64 m_generation = 0;
    bool m_silent = false;
    bool m_ready = false;
    bool m_releasing = false;
    QJsonObject m_observed;
    QJsonObject m_artifactState;
    QJsonObject m_captureMetadata;
    QJsonObject m_pngMetadata;
    std::optional<Pending> m_pending;
    std::shared_ptr<ScreenshotExportArtifact> m_artifact;
    qreal m_artifactScale = 1;
    ScreenshotClipboardCommitHandle m_clipboard;
    ScreenshotExportJobHandle m_metadataJob;
    QHash<QString, Cached> m_cache;
    QQueue<QString> m_cacheOrder;
    qsizetype m_cacheBytes = 0;
    QHash<QString, QJsonObject> m_operations;
    QQueue<QString> m_operationOrder;
    QString m_activeOperation;
    quint64 m_operationGeneration = 0;
};
} // namespace snow_shot::app::mcp
