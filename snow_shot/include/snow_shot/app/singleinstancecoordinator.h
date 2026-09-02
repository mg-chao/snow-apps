#ifndef SNOW_SHOT_APP_SINGLEINSTANCECOORDINATOR_H
#define SNOW_SHOT_APP_SINGLEINSTANCECOORDINATOR_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

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

    [[nodiscard]] SingleInstanceResult acquireOrForward(const QStringList& arguments);
    [[nodiscard]] bool isPrimary() const;
    [[nodiscard]] QString lockFilePath() const;
    void setLaunchRequestHandler(std::function<void(const QStringList&)> handler);

  signals:
    void launchRequestReceived(const QStringList& arguments);

  private:
    [[nodiscard]] bool startServer(QString* error);
    [[nodiscard]] bool forwardRequest(const QStringList& arguments, int timeoutMilliseconds,
                                      QString* error) const;
    void acceptConnections();
    void readSocket(QLocalSocket* socket);
    void dispatchRequest(QStringList arguments);

    QString m_lockFilePath;
    QString m_serverName;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QLocalServer> m_server;
    std::function<void(const QStringList&)> m_handler;
    QVector<QStringList> m_pendingRequests;
    bool m_primary = false;
};
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_SINGLEINSTANCECOORDINATOR_H
