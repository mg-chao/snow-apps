#ifndef SNOW_SHOT_APP_SINGLEINSTANCECOORDINATOR_H
#define SNOW_SHOT_APP_SINGLEINSTANCECOORDINATOR_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QQueue>
#include <QSet>

#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;

namespace snow_shot::app {
enum class SingleInstanceOutcome {
    Primary,
    Forwarded,
    Failed,
};

struct SingleInstanceResult {
    SingleInstanceOutcome outcome = SingleInstanceOutcome::Failed;
    QString error;
};

class SingleInstanceCoordinator final : public QObject {
    Q_OBJECT

  public:
    explicit SingleInstanceCoordinator(QObject* parent = nullptr);
    ~SingleInstanceCoordinator() override;

    // Modern primaries acknowledge receipt IDs on a versioned endpoint. Legacy primaries
    // retain one-way forwarding, with no retry once a write has started.
    [[nodiscard]] SingleInstanceResult acquireOrForward(const QStringList& arguments,
                                                        int forwardingTimeoutMilliseconds = 1500);
    [[nodiscard]] bool isPrimary() const;
    [[nodiscard]] QString lockFilePath() const;
    void setLaunchRequestHandler(std::function<void(const QStringList&)> handler);
    // Synchronous inspection before dispatch/receipt; must not dispatch or pump events.
    // Rejected intent is never queued or remembered.
    // Legacy senders remain one-way and cannot observe a refusal.
    void setLaunchRequestAdmission(std::function<bool(const QStringList&)> admission);
    // Reject new requests before the event loop stops, while retaining storage ownership
    // until destruction. Receipt-capable senders retry against the replacement.
    void stopAcceptingLaunchRequests();

  signals:
    void launchRequestReceived(const QStringList& arguments);

  private:
    enum class ForwardResult { Forwarded, Retry, Failed };

    [[nodiscard]] bool startServer(QString* error);
    [[nodiscard]] ForwardResult forwardRequest(const QStringList& arguments,
                                               const QString& requestId, int timeoutMilliseconds,
                                               bool& receiptRequired, QString* error) const;
    void acceptConnections(QLocalServer* server, int protocolVersion);
    void readSocket(QLocalSocket* socket, int protocolVersion);
    void dispatchRequest(QStringList arguments);

    QString m_lockFilePath;
    QString m_legacyServerName;
    QString m_receiptServerName;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QLocalServer> m_receiptServer;
    std::unique_ptr<QLocalServer> m_legacyServer;
    std::function<void(const QStringList&)> m_handler;
    std::function<bool(const QStringList&)> m_admission;
    QVector<QStringList> m_pendingRequests;
    QSet<QLocalSocket*> m_clients;
    QSet<QString> m_receivedRequestIds;
    QQueue<QString> m_requestOrder;
    bool m_primary = false;
    bool m_acceptingRequests = false;
};
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_SINGLEINSTANCECOORDINATOR_H
