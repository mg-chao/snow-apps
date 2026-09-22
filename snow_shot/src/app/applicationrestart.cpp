#include "snow_shot/app/applicationrestart.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QUuid>

#include <memory>

namespace snow_shot::app {
namespace {

constexpr auto kHelperArgument = "--restart-helper";
constexpr int kHandoffTimeoutMs = 30000;

QString translated(const char* source) {
    return QCoreApplication::translate("ApplicationRestart", source);
}

ApplicationRestartResult failure(const QString& error) {
    return {false, error};
}

bool writeLine(QLocalSocket& socket, const QByteArray& line) {
    if (socket.write(line + '\n') < 0) {
        return false;
    }
    return socket.bytesToWrite() == 0 || socket.waitForBytesWritten(10000);
}

QByteArray readLine(QLocalSocket& socket, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!socket.canReadLine()) {
        const qint64 remaining = static_cast<qint64>(timeoutMs) - timer.elapsed();
        if (remaining <= 0 || !socket.waitForReadyRead(static_cast<int>(remaining))) {
            return {};
        }
    }
    return socket.readLine().trimmed();
}

class RestartHandoff final : public QObject {
  public:
    explicit RestartHandoff(QCoreApplication& application)
        : QObject(&application), server(new QLocalServer(this)) {
        server->setSocketOptions(QLocalServer::UserAccessOption);
    }

    ApplicationRestartResult prepare() {
        name = QStringLiteral("snow-shot-restart-") + QUuid::createUuid().toString(QUuid::Id128);
        nonce = QUuid::createUuid().toString(QUuid::Id128).toLatin1();
        if (!server->listen(name)) {
            return failure(translated(
                QT_TRANSLATE_NOOP("ApplicationRestart", "Could not create the restart handoff.")));
        }

        const QStringList arguments{QString::fromLatin1(kHelperArgument), name,
                                    QString::fromLatin1(nonce)};
        qint64 processId = 0;
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments,
                                     QCoreApplication::applicationDirPath(), &processId) ||
            processId <= 0) {
            return failure(translated(QT_TRANSLATE_NOOP(
                "ApplicationRestart", "Could not start the replacement application.")));
        }

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kHandoffTimeoutMs) {
            const int remaining = kHandoffTimeoutMs - static_cast<int>(timer.elapsed());
            if (!server->hasPendingConnections() && !server->waitForNewConnection(remaining)) {
                break;
            }
            std::unique_ptr<QLocalSocket> candidate(server->nextPendingConnection());
            if (candidate == nullptr) {
                continue;
            }
            const QByteArray expected = QByteArrayLiteral("ready ") + nonce;
            if (readLine(*candidate, remaining) == expected) {
                socket = candidate.release();
                socket->setParent(this);
                return {true, {}};
            }
            candidate->disconnectFromServer();
        }
        return failure(translated(QT_TRANSLATE_NOOP(
            "ApplicationRestart", "The replacement application did not become ready.")));
    }

    ApplicationRestartResult commit() {
        if (socket == nullptr || !writeLine(*socket, QByteArrayLiteral("commit")) ||
            readLine(*socket, 10000) != QByteArrayLiteral("committed")) {
            return failure(translated(
                QT_TRANSLATE_NOOP("ApplicationRestart",
                                  "The replacement application did not acknowledge the restart.")));
        }
        committed = true;
        return {true, {}};
    }

    void cancel() {
        if (socket == nullptr || committed) {
            return;
        }
        static_cast<void>(writeLine(*socket, QByteArrayLiteral("cancel")));
        static_cast<void>(readLine(*socket, 5000));
        socket->disconnectFromServer();
    }

  private:
    QLocalServer* server = nullptr;
    QLocalSocket* socket = nullptr;
    QString name;
    QByteArray nonce;
    bool committed = false;
};

} // namespace

ApplicationRestartResult
runApplicationRestartTransaction(const ApplicationRestartTransactionOperations& operations) {
    if (!operations.guard || !operations.guard()) {
        return failure(translated(QT_TRANSLATE_NOOP(
            "ApplicationRestart",
            "Finish capturing, recording, exporting, or updating before restarting.")));
    }
    const ApplicationRestartResult prepared = operations.prepare();
    if (!prepared.success) {
        return prepared;
    }
    const auto cancel = [&operations]() {
        if (operations.cancel) {
            operations.cancel();
        }
    };
    if (!operations.guard()) {
        cancel();
        return failure(translated(QT_TRANSLATE_NOOP(
            "ApplicationRestart",
            "Finish capturing, recording, exporting, or updating before restarting.")));
    }
    if (!operations.flush || !operations.flush()) {
        cancel();
        return failure(translated(QT_TRANSLATE_NOOP(
            "ApplicationRestart",
            "Your settings could not be saved. Please retry before restarting.")));
    }
    const ApplicationRestartResult committed = operations.commit();
    if (!committed.success) {
        cancel();
        return committed;
    }
    if (operations.quit) {
        operations.quit();
    }
    return {true, {}};
}

ApplicationRestartCoordinator::ApplicationRestartCoordinator(QCoreApplication& application)
    : m_application(application) {}

ApplicationRestartResult
ApplicationRestartCoordinator::restart(const std::function<bool()>& guard,
                                       const std::function<bool()>& flush) {
    if (m_pending) {
        return failure(translated(
            QT_TRANSLATE_NOOP("ApplicationRestart", "Another restart operation is in progress.")));
    }
    m_pending = true;
    auto handoff = std::make_unique<RestartHandoff>(m_application);
    const ApplicationRestartResult result = runApplicationRestartTransaction(
        {[&handoff]() { return handoff->prepare(); }, guard, flush,
         [&handoff]() { return handoff->commit(); }, [&handoff]() { handoff->cancel(); },
         []() { QCoreApplication::quit(); }});
    if (result.success) {
        // The child waits for this QObject-owned connection to close. Parenting the handoff to
        // QApplication keeps it alive until after stack-owned single-instance coordination dies.
        static_cast<void>(handoff.release());
    } else {
        m_pending = false;
    }
    return result;
}

bool isApplicationRestartHelperLaunch(const QStringList& arguments) {
    return arguments.size() >= 2 && arguments.at(1) == QString::fromLatin1(kHelperArgument);
}

QStringList normalApplicationArguments(const QStringList& arguments) {
    if (!isApplicationRestartHelperLaunch(arguments)) {
        return arguments;
    }
    return arguments.isEmpty() ? QStringList{} : QStringList{arguments.first()};
}

int dispatchApplicationRestartHelper(const QStringList& arguments) {
    if (!isApplicationRestartHelperLaunch(arguments)) {
        return -1;
    }
    if (arguments.size() != 4 || !arguments.at(2).startsWith(u"snow-shot-restart-") ||
        arguments.at(3).size() != 32) {
        return 30;
    }

    QLocalSocket socket;
    socket.connectToServer(arguments.at(2));
    if (!socket.waitForConnected(10000)) {
        return 31;
    }
    const QByteArray nonce = arguments.at(3).toLatin1();
    if (!writeLine(socket, QByteArrayLiteral("ready ") + nonce)) {
        return 32;
    }
    const QByteArray command = readLine(socket, kHandoffTimeoutMs);
    if (command == QByteArrayLiteral("cancel")) {
        static_cast<void>(writeLine(socket, QByteArrayLiteral("cancelled")));
        return 0;
    }
    if (command != QByteArrayLiteral("commit") ||
        !writeLine(socket, QByteArrayLiteral("committed"))) {
        return 33;
    }

    // The parent owns the server-side socket until QApplication destruction. That happens after
    // its stack-owned single-instance coordinator releases the application lock.
    while (socket.state() == QLocalSocket::ConnectedState) {
        socket.waitForDisconnected(-1);
    }
    return -1;
}

} // namespace snow_shot::app
