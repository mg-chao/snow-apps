#ifndef SNOW_SHOT_APP_MCP_SCREENSHOTMCPSERVER_H
#define SNOW_SHOT_APP_MCP_SCREENSHOTMCPSERVER_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <memory>
#include <optional>

class QLockFile;

namespace snow_shot::app::mcp {

struct ScreenshotMcpRequest final {
    quint64 connectionId = 0;
    QString requestId;
    QString method;
    QString sessionId;
    std::optional<quint64> expectedRevision;
    QString idempotencyKey;
    QJsonObject params;
    QByteArray attachment;
};

struct ScreenshotMcpResponse final {
    bool ok = false;
    QString requestId;
    QString sessionId;
    std::optional<quint64> revision;
    QJsonObject result;
    QString errorCode;
    QString errorMessage;
    QJsonObject errorDetails;
    QString attachmentMime;
    QByteArray attachment;
    // Application lifecycle actions run only after the complete reply has left the socket queue.
    std::function<void()> afterSend;
};

class ScreenshotMcpServer final : public QObject {
    Q_OBJECT

  public:
    using Completion = std::function<void(ScreenshotMcpResponse)>;
    using RequestHandler = std::function<void(const ScreenshotMcpRequest&, Completion)>;
    using ClientDisconnectedHandler = std::function<void(quint64)>;
    using RequestCancellationHandler = std::function<bool(quint64, const QString&)>;

    explicit ScreenshotMcpServer(QObject* parent = nullptr, QString runtimeDirectory = {});
    [[nodiscard]] static QString defaultRuntimeDirectory();
    ~ScreenshotMcpServer() override;

    ScreenshotMcpServer(const ScreenshotMcpServer&) = delete;
    ScreenshotMcpServer& operator=(const ScreenshotMcpServer&) = delete;

    [[nodiscard]] bool start(QString* error = nullptr);
    void stop();
    void drainAndStop(std::function<void()> afterDrain = {}, int timeoutMilliseconds = 2000);
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString descriptorPath() const;
    [[nodiscard]] QString socketName() const;

    void setRequestHandler(RequestHandler handler);
    void setClientDisconnectedHandler(ClientDisconnectedHandler handler);
    void setRequestCancellationHandler(RequestCancellationHandler handler);
    void publishEvent(quint64 connection, QJsonObject event);

  signals:
    void runningChanged(bool running);
    void protocolError(const QString& message);

  private:
    class SocketWorker;

    void handleRequest(quint64 connectionId, const QJsonObject& request,
                       const QByteArray& attachment);
    void handleClientDisconnected(quint64 connectionId);
    void sendResponse(quint64 connectionId, ScreenshotMcpResponse response);
    [[nodiscard]] bool writeDescriptor(QString* error);
    void removeDescriptor();

    std::unique_ptr<QThread> m_thread;
    std::unique_ptr<QLockFile> m_lock;
    SocketWorker* m_worker = nullptr;
    RequestHandler m_requestHandler;
    ClientDisconnectedHandler m_clientDisconnectedHandler;
    RequestCancellationHandler m_requestCancellationHandler;
    QString m_runtimeDirectory;
    QString m_generation;
    QString m_descriptorPath;
    QString m_socketName;
    QString m_token;
    bool m_running = false;
    quint64 m_epoch = 0;
};

} // namespace snow_shot::app::mcp

#endif // SNOW_SHOT_APP_MCP_SCREENSHOTMCPSERVER_H
