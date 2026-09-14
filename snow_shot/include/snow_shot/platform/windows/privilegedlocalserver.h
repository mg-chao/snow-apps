#pragma once

#include <QLocalServer>
#include <memory>

namespace snow_shot::platform::windows {
// An updater worker can run under the alternate administrator supplied to UAC.
// Qt's UserAccessOption cannot express owner + Administrators access on Windows.
// Accepted connections still require executable/PID verification by the caller.
class PrivilegedLocalServer final : public QObject {
    Q_OBJECT
  public:
    explicit PrivilegedLocalServer(QObject* parent = nullptr);
    ~PrivilegedLocalServer() override;
    bool listen(const QString& name);
    void close();
    QLocalSocket* nextPendingConnection();
  signals:
    void newConnection();
    void acceptError(QAbstractSocket::SocketError error);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::platform::windows
