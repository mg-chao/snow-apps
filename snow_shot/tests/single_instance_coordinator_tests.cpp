#include "snow_shot/app/singleinstancecoordinator.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <QUuid>

#include <cstdlib>
#include <iostream>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace single_instance = snow_shot::app;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void writeReadyFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "failed to create child readiness marker");
    require(file.write("ready") == 5, "failed to write child readiness marker");
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMilliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

QLocalServer* launchServer(single_instance::SingleInstanceCoordinator& primary,
                           int protocolVersion = 2) {
    QString name = QFileInfo(primary.lockFilePath()).completeBaseName();
    if (protocolVersion == 2)
        name += QStringLiteral("-v2");
    for (auto* server : primary.findChildren<QLocalServer*>())
        if (server->serverName() == name)
            return server;
    require(false, "primary exposes the expected launch protocol endpoint");
    return nullptr;
}

void forwardingToLegacyPrimaryDispatchesOnce() {
    single_instance::SingleInstanceCoordinator identity;
    QLockFile owner(identity.lockFilePath());
    owner.setStaleLockTime(0);
    require(owner.tryLock(0), "legacy primary acquires storage ownership");
    QLocalServer legacy;
    legacy.setSocketOptions(QLocalServer::UserAccessOption);
    require(legacy.listen(QFileInfo(identity.lockFilePath()).completeBaseName()),
            "legacy primary listens on the original version-1 endpoint");
    int received = 0;
    QStringList receivedArguments;
    // The original version-1 primary dispatches requests without writing any response.
    QObject::connect(&legacy, &QLocalServer::newConnection, &legacy, [&] {
        while (legacy.hasPendingConnections()) {
            auto* socket = legacy.nextPendingConnection();
            const auto read = [&, socket] {
                auto buffer = socket->property("legacyLaunchBuffer").toByteArray();
                buffer.append(socket->readAll());
                if (buffer.size() >= static_cast<qsizetype>(sizeof(quint32))) {
                    const auto size = qFromBigEndian<quint32>(buffer.constData());
                    if (buffer.size() >= static_cast<qsizetype>(sizeof(quint32)) + size) {
                        const auto request =
                            QJsonDocument::fromJson(buffer.mid(sizeof(quint32), size)).object();
                        require(request.value(QStringLiteral("version")).toInt() == 1,
                                "legacy primary receives only the version-1 launch protocol");
                        ++received;
                        for (const auto& value :
                             request.value(QStringLiteral("arguments")).toArray())
                            receivedArguments.append(value.toString());
                        buffer.remove(0, static_cast<qsizetype>(sizeof(quint32)) + size);
                    }
                }
                socket->setProperty("legacyLaunchBuffer", buffer);
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, read);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            read();
        }
    });
    const QStringList arguments{QStringLiteral("snow-shot-test"),
                                QStringLiteral("--show-main-window"),
                                QStringLiteral("--edit-with-snow-shot"),
                                QDir::temp().filePath(QStringLiteral("image one 雪.png"))};
    QProcess secondary;
    auto childArguments = arguments;
    childArguments.prepend(QStringLiteral("--single-instance-forward"));
    secondary.start(QCoreApplication::applicationFilePath(), childArguments);
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "client finishes forwarding to the legacy primary");
    if (secondary.exitCode() != EXIT_SUCCESS)
        std::cerr << secondary.readAllStandardError().constData();
    require(
        secondary.exitStatus() == QProcess::NormalExit && secondary.exitCode() == EXIT_SUCCESS &&
            received == 1 && receivedArguments == arguments,
        "a legacy launch is forwarded successfully and dispatched exactly once without a receipt");
}

#ifdef Q_OS_WIN
class UnresponsiveLegacyServer final : public QLocalServer {
  public:
    ~UnresponsiveLegacyServer() override {
        for (const auto descriptor : m_connections)
            CloseHandle(reinterpret_cast<HANDLE>(descriptor));
    }
    [[nodiscard]] qsizetype connectionCount() const {
        return m_connections.size();
    }

  protected:
    void incomingConnection(quintptr descriptor) override {
        // Keep the native pipe open without constructing Qt's automatically draining reader.
        m_connections.append(descriptor);
    }

  private:
    QList<quintptr> m_connections;
};

void uncertainLegacyWriteIsNotRetried() {
    single_instance::SingleInstanceCoordinator identity;
    QLockFile owner(identity.lockFilePath());
    owner.setStaleLockTime(0);
    require(owner.tryLock(0), "legacy sink acquires storage ownership");
    UnresponsiveLegacyServer legacy;
    legacy.setSocketOptions(QLocalServer::UserAccessOption);
    require(legacy.listen(QFileInfo(identity.lockFilePath()).completeBaseName()),
            "legacy sink listens on the version-1 endpoint");
    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-large-forward")});
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "client finishes the blocked legacy write");
    if (secondary.exitCode() != EXIT_SUCCESS)
        std::cerr << secondary.readAllStandardError().constData();
    require(secondary.exitStatus() == QProcess::NormalExit &&
                secondary.exitCode() == EXIT_SUCCESS && legacy.connectionCount() == 1,
            "an uncertain legacy write fails after one attempt without reconnecting or resending");
}
#endif

void firstInstanceAndQueuedForwarding() {
    single_instance::SingleInstanceCoordinator primary;
    const auto acquired = primary.acquireOrForward({QStringLiteral("snow-shot-test")});
    require(acquired.outcome == single_instance::SingleInstanceOutcome::Primary &&
                primary.isPrimary(),
            "first process did not acquire single-instance ownership");

    QStringList signaledArguments;
    QObject::connect(
        &primary, &single_instance::SingleInstanceCoordinator::launchRequestReceived,
        [&signaledArguments](const QStringList& arguments) { signaledArguments = arguments; });
    const QStringList forwardedArguments{QStringLiteral("snow-shot-test"),
                                         QStringLiteral("--show-main-window"),
                                         QStringLiteral("capture.png")};
    QProcess secondary;
    QStringList secondaryArguments{QStringLiteral("--single-instance-forward")};
    secondaryArguments.append(forwardedArguments);
    secondary.start(QCoreApplication::applicationFilePath(), secondaryArguments);
    require(secondary.waitForStarted(3000), "second process did not start");
    // Process primary events while the child retries its forward so the local
    // server can accept and drain the request exactly as it does in production.
    require(waitUntil([&secondary]() { return secondary.state() == QProcess::NotRunning; }, 5000),
            "second process did not finish its forwarding attempt");
    if (secondary.exitStatus() != QProcess::NormalExit || secondary.exitCode() != EXIT_SUCCESS) {
        std::cerr << secondary.readAllStandardError().constData();
    }
    require(secondary.exitStatus() == QProcess::NormalExit && secondary.exitCode() == EXIT_SUCCESS,
            "second process did not forward its launch request");
    require(waitUntil([&signaledArguments]() { return !signaledArguments.isEmpty(); }, 1000) &&
                signaledArguments == forwardedArguments,
            "primary did not decode the forwarded launch request");

    QStringList handledArguments;
    primary.setLaunchRequestHandler(
        [&handledArguments](const QStringList& arguments) { handledArguments = arguments; });
    require(handledArguments == forwardedArguments,
            "launch request received before controller readiness was not queued");
}

void staleOwnerIsRecovered() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create stale-owner directory");
    const QString readyPath = temporary.filePath(QStringLiteral("ready"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--single-instance-stale-owner"), readyPath});
    require(child.waitForStarted(3000) &&
                waitUntil([&readyPath]() { return QFileInfo::exists(readyPath); }, 3000),
            "stale-owner child did not acquire the instance");
    child.kill();
    require(child.waitForFinished(3000), "stale-owner child did not terminate");

    single_instance::SingleInstanceCoordinator recovered;
    const auto result = recovered.acquireOrForward({QStringLiteral("snow-shot-test")});
    require(result.outcome == single_instance::SingleInstanceOutcome::Primary &&
                recovered.isPrimary(),
            "Qt-identified stale lock was not recovered");
}

void liveUnreachableOwnerIsNotBypassed() {
    single_instance::SingleInstanceCoordinator probe;
    QLockFile liveLock(probe.lockFilePath());
    liveLock.setStaleLockTime(0);
    require(liveLock.tryLock(0), "failed to create live unreachable lock fixture");

    single_instance::SingleInstanceCoordinator contender;
    QElapsedTimer timer;
    timer.start();
    const auto result = contender.acquireOrForward({QStringLiteral("snow-shot-test")});
    require(result.outcome == single_instance::SingleInstanceOutcome::Failed &&
                !contender.isPrimary() && !result.error.isEmpty() && timer.elapsed() >= 1400,
            "live unreachable owner was bypassed or not retried for the IPC window");
    liveLock.unlock();
}

void stoppingPrimaryRetainsLockAndAllowsRetry() {
    auto primary = std::make_unique<single_instance::SingleInstanceCoordinator>();
    require(primary->acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the stopping-primary fixture");
    const auto endpoints = primary->findChildren<QLocalServer*>();
    require(endpoints.size() == 2, "modern primary publishes both launch protocol endpoints");
    primary->stopAcceptingLaunchRequests();
    for (auto* endpoint : endpoints)
        require(!endpoint->isListening(), "stopping primary closes both launch protocol endpoints");
    QLockFile storageLock(primary->lockFilePath());
    require(!storageLock.tryLock(0), "stopping primary retains storage ownership until exit");
    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-become-primary")});
    require(secondary.waitForStarted(3000), "replacement contender did not start");
    QTimer::singleShot(250, [&] { primary.reset(); });
    require(waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "replacement contender did not finish");
    if (secondary.exitCode() != EXIT_SUCCESS)
        std::cerr << secondary.readAllStandardError().constData();
    require(secondary.exitStatus() == QProcess::NormalExit && secondary.exitCode() == EXIT_SUCCESS,
            "unacknowledged launch retries ownership after the stopping primary exits");
}

QByteArray requestFrame(const QString& id, int protocolVersion = 2,
                        const QStringList& arguments = {QStringLiteral("snow-shot-test"),
                                                        QStringLiteral("--show-main-window")}) {
    QJsonObject request{
        {QStringLiteral("version"), protocolVersion},
        {QStringLiteral("activate"), true},
        {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
    };
    if (!id.isEmpty())
        request.insert(QStringLiteral("requestId"), id);
    const auto payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.resize(sizeof(quint32));
    qToBigEndian(static_cast<quint32>(payload.size()), frame.data());
    frame.append(payload);
    return frame;
}

void repeatedRequestReceiptsDoNotRepeatDispatch() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the repeated-receipt fixture");
    int received = 0;
    QObject::connect(&primary, &single_instance::SingleInstanceCoordinator::launchRequestReceived,
                     [&] { ++received; });
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128);
    for (const int version : {1, 2}) {
        QProcess secondary;
        secondary.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--single-instance-repeat-request"),
                         launchServer(primary, version)->serverName(), requestId,
                         QString::number(version)});
        require(secondary.waitForStarted(3000) &&
                    waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
                "repeated-receipt client did not finish");
        require(secondary.exitStatus() == QProcess::NormalExit &&
                    secondary.exitCode() == EXIT_SUCCESS && received == 1,
                "the same request ID dispatches once across retries and both protocol endpoints");
    }
}

void refusedIntentIsNotDispatchedOrRemembered() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "admission fixture owns storage");
    bool allow = false;
    int admissions = 0;
    int received = 0;
    primary.setLaunchRequestAdmission([&](const QStringList&) {
        ++admissions;
        return allow;
    });
    QObject::connect(&primary, &single_instance::SingleInstanceCoordinator::launchRequestReceived,
                     [&] { ++received; });
    const auto send = [&](const QString& id) {
        QLocalSocket socket;
        socket.connectToServer(launchServer(primary)->serverName(), QIODevice::ReadWrite);
        require(socket.waitForConnected(1000), "admission client connects");
        const auto request = requestFrame(id, 2);
        require(socket.write(request) == request.size(), "admission client writes");
        require(waitUntil(
                    [&] {
                        if (socket.bytesAvailable() < static_cast<qint64>(sizeof(quint32)))
                            return false;
                        const auto header = socket.peek(sizeof(quint32));
                        return socket.bytesAvailable() >=
                               static_cast<qint64>(sizeof(quint32)) +
                                   qFromBigEndian<quint32>(header.constData());
                    },
                    1000),
                "admission returns a receipt");
        const auto response = socket.readAll();
        require(response.size() > static_cast<qsizetype>(sizeof(quint32)), "receipt is framed");
        return QJsonDocument::fromJson(response.mid(sizeof(quint32)))
            .object()
            .value(QStringLiteral("accepted"))
            .toBool();
    };
    require(!send(QStringLiteral("capacity-request")) && received == 0,
            "refused intent receives a negative receipt without dispatch");
    allow = true;
    require(send(QStringLiteral("capacity-request")) && received == 1,
            "a refused ID is eligible for later admission");
    allow = false;
    require(send(QStringLiteral("capacity-request")) && received == 1 && admissions == 2,
            "an accepted retry keeps its receipt even after admission closes");
    int queued = 0;
    primary.setLaunchRequestHandler([&](const QStringList&) { ++queued; });
    require(queued == 1, "only accepted intent survives for deferred dispatch");

    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-refused-forward")});
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000) &&
                secondary.exitStatus() == QProcess::NormalExit && secondary.exitCode() == 0 &&
                admissions == 3 && received == 1,
            "a modern negative receipt fails once without retry or legacy downgrade");
}

void legacyClientCanForwardToModernPrimary() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "modern primary acquires storage ownership");
    const QStringList arguments{QStringLiteral("snow-shot-test"),
                                QStringLiteral("--show-main-window")};
    int received = 0;
    QStringList receivedArguments;
    primary.setLaunchRequestHandler([&](const QStringList& request) {
        ++received;
        receivedArguments = request;
    });
    QProcess secondary;
    QStringList childArguments{QStringLiteral("--single-instance-legacy-forward"),
                               launchServer(primary, 1)->serverName()};
    childArguments.append(arguments);
    secondary.start(QCoreApplication::applicationFilePath(), childArguments);
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "legacy client finishes forwarding");
    require(secondary.exitStatus() == QProcess::NormalExit &&
                secondary.exitCode() == EXIT_SUCCESS && received == 1 &&
                receivedArguments == arguments,
            "a modern primary retains one-way version-1 forwarding for older clients");
}

void modernEndpointRequiresVersionAndReceiptId() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "primary acquires the modern request validation fixture");
    int received = 0;
    primary.setLaunchRequestHandler([&](const QStringList&) { ++received; });
    for (const int version : {1, 2}) {
        QProcess secondary;
        secondary.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--single-instance-invalid-request"),
                         launchServer(primary)->serverName(), QString::number(version),
                         version == 1 ? QStringLiteral("legacy-request") : QString()});
        require(secondary.waitForStarted(3000) &&
                    waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
                "invalid modern request client finishes");
        require(secondary.exitStatus() == QProcess::NormalExit &&
                    secondary.exitCode() == EXIT_SUCCESS && received == 0,
                "receipt endpoint rejects legacy versions and missing receipt IDs before dispatch");
    }
}

void largeModernRequestIsFullyWritten() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "primary acquires the large modern request fixture");
    int received = 0;
    primary.setLaunchRequestHandler([&](const QStringList& arguments) {
        require(arguments == QStringList{QStringLiteral("snow-shot-test"), QString(900000, u'x')},
                "the complete launch frame reaches the modern primary without truncated arguments");
        ++received;
    });
    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-large-modern-forward")});
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "large modern request client finishes");
    if (secondary.exitCode() != EXIT_SUCCESS)
        std::cerr << secondary.readAllStandardError().constData();
    require(secondary.exitStatus() == QProcess::NormalExit &&
                secondary.exitCode() == EXIT_SUCCESS && received == 1,
            "large modern requests receive a receipt after exactly one complete dispatch");
}

void disappearingReceiptEndpointDoesNotDowngrade() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "primary acquires the disappearing receipt endpoint fixture");
    auto* original = launchServer(primary);
    const QString name = original->serverName();
    original->close();
    QLocalServer disappearing;
    disappearing.setSocketOptions(QLocalServer::UserAccessOption);
    require(disappearing.listen(name), "receipt endpoint initially advertises modern support");
    int received = 0;
    primary.setLaunchRequestHandler([&](const QStringList&) { ++received; });
    QObject::connect(&disappearing, &QLocalServer::newConnection, &disappearing, [&] {
        auto* socket = disappearing.nextPendingConnection();
        // The legacy endpoint remains reachable, but cannot replace a missing modern receipt.
        disappearing.close();
        socket->abort();
        socket->deleteLater();
    });
    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-forward"), QStringLiteral("snow-shot-test"),
                     QStringLiteral("--show-main-window")});
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "client finishes after receipt endpoint disappears");
    require(secondary.exitStatus() == QProcess::NormalExit &&
                secondary.exitCode() == EXIT_FAILURE && received == 0,
            "receipt support stays required across retries after its endpoint disappears");
}

void invalidReceiptsDoNotDowngrade() {
    for (const auto& invalidField :
         {QStringLiteral("version"), QStringLiteral("requestId"), QStringLiteral("accepted")}) {
        single_instance::SingleInstanceCoordinator primary;
        require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                    single_instance::SingleInstanceOutcome::Primary,
                "primary acquires the invalid receipt fixture");
        auto* original = launchServer(primary);
        const QString name = original->serverName();
        original->close();
        QLocalServer invalid;
        invalid.setSocketOptions(QLocalServer::UserAccessOption);
        require(invalid.listen(name), "invalid receipt endpoint advertises modern support");
        int legacyRequests = 0;
        int modernRequests = 0;
        primary.setLaunchRequestHandler([&](const QStringList&) { ++legacyRequests; });
        QObject::connect(&invalid, &QLocalServer::newConnection, &invalid, [&] {
            auto* socket = invalid.nextPendingConnection();
            const auto read = [&, socket] {
                auto buffer = socket->property("requestBuffer").toByteArray();
                buffer.append(socket->readAll());
                socket->setProperty("requestBuffer", buffer);
                if (buffer.size() < static_cast<qsizetype>(sizeof(quint32)))
                    return;
                const auto size = qFromBigEndian<quint32>(buffer.constData());
                if (buffer.size() < static_cast<qsizetype>(sizeof(quint32)) + size)
                    return;
                const auto request =
                    QJsonDocument::fromJson(buffer.mid(sizeof(quint32), size)).object();
                require(request.value(QStringLiteral("version")).toInt() == 2 &&
                            !request.value(QStringLiteral("requestId")).toString().isEmpty(),
                        "modern endpoint receives a version-2 request with a receipt ID");
                ++modernRequests;
                QJsonObject response{
                    {QStringLiteral("version"), 2},
                    {QStringLiteral("accepted"), true},
                    {QStringLiteral("requestId"), request.value(QStringLiteral("requestId"))},
                };
                if (invalidField == u"version")
                    response.insert(invalidField, 1);
                else if (invalidField == u"requestId")
                    response.insert(invalidField, QStringLiteral("another-request"));
                else
                    response.insert(invalidField, false);
                const auto payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
                QByteArray receipt(sizeof(quint32), '\0');
                qToBigEndian(static_cast<quint32>(payload.size()), receipt.data());
                receipt.append(payload);
                socket->write(receipt);
                socket->disconnectFromServer();
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, read);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            read();
        });
        QProcess secondary;
        secondary.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--single-instance-forward"),
                         QStringLiteral("snow-shot-test"), QStringLiteral("--show-main-window")});
        require(secondary.waitForStarted(3000) &&
                    waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
                "client finishes after an invalid modern receipt");
        require(secondary.exitStatus() == QProcess::NormalExit &&
                    secondary.exitCode() == EXIT_FAILURE && modernRequests > 0 &&
                    legacyRequests == 0,
                "invalid version, ID, or acceptance cannot acknowledge or downgrade a launch");
    }
}

void customTimeoutWaitsForColdPrimaryReadiness() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire cold-primary readiness fixture");
    QTemporaryDir directory;
    const QString ready = directory.filePath(QStringLiteral("ready"));
    const QString begin = directory.filePath(QStringLiteral("begin"));
    QProcess secondary;
    secondary.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--single-instance-custom-timeout"), QStringLiteral("2200"), ready, begin});
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return QFileInfo::exists(ready); }, 3000),
            "custom-timeout secondary did not reach forwarding readiness");
    writeReadyFile(begin);
    // The explicit 2.2 s window covers a primary whose event loop becomes ready after the
    // ordinary 1.5 s forwarding window, without extending the default unreachable-owner wait.
    QThread::msleep(1600);
    require(waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 3000),
            "custom-timeout secondary did not finish after primary readiness");
    if (secondary.exitCode() != EXIT_SUCCESS)
        std::cerr << secondary.readAllStandardError().constData();
    require(secondary.exitStatus() == QProcess::NormalExit && secondary.exitCode() == EXIT_SUCCESS,
            "configured shell forwarding window survives delayed cold-primary readiness");
}

#ifdef Q_OS_WIN
class DelayedAcceptServer final : public QLocalServer {
  protected:
    void incomingConnection(quintptr descriptor) override {
        // Keep the client's write pending longer than Qt's 10 ms pipe poll.
        QTimer::singleShot(100, this,
                           [this, descriptor] { QLocalServer::incomingConnection(descriptor); });
    }
};

void acknowledgeSocket(QLocalSocket* socket) {
    auto buffer = socket->property("receivedFrame").toByteArray();
    buffer.append(socket->readAll());
    socket->setProperty("receivedFrame", buffer);
    if (buffer.size() < static_cast<qsizetype>(sizeof(quint32)))
        return;
    const auto size = qFromBigEndian<quint32>(buffer.constData());
    if (buffer.size() < static_cast<qsizetype>(sizeof(quint32)) + size)
        return;
    const auto request = QJsonDocument::fromJson(buffer.mid(sizeof(quint32), size)).object();
    const auto payload =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("version"), request.value(QStringLiteral("version"))},
                          {QStringLiteral("accepted"), true},
                          {QStringLiteral("requestId"), request.value(QStringLiteral("requestId"))},
                      })
            .toJson(QJsonDocument::Compact);
    QByteArray receipt;
    receipt.resize(sizeof(quint32));
    qToBigEndian(static_cast<quint32>(payload.size()), receipt.data());
    receipt.append(payload);
    QTimer::singleShot(100, socket, [socket, receipt] {
        socket->write(receipt);
        socket->flush();
    });
}

void forwardingWaitsForSlowPrimary() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the slow-primary fixture");
    auto* originalServer = launchServer(primary);
    require(originalServer != nullptr, "primary has no IPC server");
    const auto name = originalServer->serverName();
    originalServer->close();
    DelayedAcceptServer delayed;
    delayed.setSocketOptions(QLocalServer::UserAccessOption);
    require(delayed.listen(name), "could not listen as the slow primary");
    QObject::connect(&delayed, &QLocalServer::newConnection, &delayed, [&] {
        auto* socket = delayed.nextPendingConnection();
        QObject::connect(socket, &QLocalSocket::readyRead, socket,
                         [socket] { acknowledgeSocket(socket); });
        acknowledgeSocket(socket);
    });
    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-forward"), QStringLiteral("snow-shot-test"),
                     QStringLiteral("--show-main-window")});
    require(secondary.waitForStarted(3000), "slow-primary client did not start");
    require(waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "slow-primary client did not finish");
    if (secondary.exitCode() != EXIT_SUCCESS) {
        std::cerr << secondary.readAllStandardError().constData();
    }
    require(secondary.exitStatus() == QProcess::NormalExit && secondary.exitCode() == EXIT_SUCCESS,
            "launch forwarding failed while the primary was slow to accept");
}

void forwardingRequiresReceipt() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the missing-receipt fixture");
    auto* originalServer = launchServer(primary);
    const QString name = originalServer->serverName();
    originalServer->close();
    QLocalServer silent;
    silent.setSocketOptions(QLocalServer::UserAccessOption);
    require(silent.listen(name), "could not listen without dispatching requests");
    int received = 0;
    primary.setLaunchRequestHandler([&](const QStringList&) { ++received; });
    QProcess secondary;
    secondary.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--single-instance-forward"), QStringLiteral("snow-shot-test"),
                     QStringLiteral("--show-main-window")});
    require(secondary.waitForStarted(3000) &&
                waitUntil([&] { return secondary.state() == QProcess::NotRunning; }, 5000),
            "missing-receipt client did not finish");
    require(secondary.exitStatus() == QProcess::NormalExit &&
                secondary.exitCode() == EXIT_FAILURE && received == 0,
            "missing modern receipts fail without resending through the live legacy endpoint");
}

void pipeRejectsAnonymousClients() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the pipe-permissions fixture");
    // Unlike the default named-pipe ACL, our same-user endpoint must not grant
    // anonymous/Everyone access. This also guards against fixing elevation by
    // making the single-instance pipe world-accessible.
    for (const int version : {1, 2}) {
        const auto name = launchServer(primary, version)->fullServerName().toStdWString();
        require(ImpersonateAnonymousToken(GetCurrentThread()),
                "could not impersonate an anonymous pipe client");
        const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED, nullptr);
        const DWORD error = GetLastError();
        const BOOL reverted = RevertToSelf();
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
        require(reverted, "could not restore the test thread token");
        require(pipe == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
                "single-instance pipe allowed an anonymous client");
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("single-instance-coordinator"));
    QCoreApplication application(argc, argv);
    if (application.arguments().value(1) == QStringLiteral("--single-instance-refused-forward")) {
        single_instance::SingleInstanceCoordinator secondary;
        const auto result = secondary.acquireOrForward({QStringLiteral("snow-shot-test")});
        return result.outcome == single_instance::SingleInstanceOutcome::Failed &&
                       !result.error.isEmpty()
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (application.arguments().value(1) == QStringLiteral("--single-instance-large-forward") ||
        application.arguments().value(1) ==
            QStringLiteral("--single-instance-large-modern-forward")) {
        single_instance::SingleInstanceCoordinator secondary;
        const auto result =
            secondary.acquireOrForward({QStringLiteral("snow-shot-test"), QString(900000, u'x')});
        const auto expected =
            application.arguments().value(1) == QStringLiteral("--single-instance-large-forward")
                ? single_instance::SingleInstanceOutcome::Failed
                : single_instance::SingleInstanceOutcome::Forwarded;
        if (result.outcome != expected)
            std::cerr << result.error.toStdString() << '\n';
        return result.outcome == expected &&
                       (expected != single_instance::SingleInstanceOutcome::Failed ||
                        !result.error.isEmpty())
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (application.arguments().value(1) == QStringLiteral("--single-instance-custom-timeout")) {
        writeReadyFile(application.arguments().value(3));
        require(
            waitUntil([&] { return QFileInfo::exists(application.arguments().value(4)); }, 3000),
            "custom-timeout client received its controlled start marker");
        single_instance::SingleInstanceCoordinator secondary;
        const auto result = secondary.acquireOrForward({QStringLiteral("snow-shot-test")},
                                                       application.arguments().value(2).toInt());
        if (result.outcome != single_instance::SingleInstanceOutcome::Forwarded)
            std::cerr << result.error.toStdString() << '\n';
        return result.outcome == single_instance::SingleInstanceOutcome::Forwarded ? EXIT_SUCCESS
                                                                                   : EXIT_FAILURE;
    }
    if (application.arguments().value(1) == QStringLiteral("--single-instance-become-primary")) {
        single_instance::SingleInstanceCoordinator secondary;
        const auto result = secondary.acquireOrForward({QStringLiteral("snow-shot-test")});
        if (result.outcome != single_instance::SingleInstanceOutcome::Primary)
            std::cerr << result.error.toStdString() << '\n';
        return result.outcome == single_instance::SingleInstanceOutcome::Primary ? EXIT_SUCCESS
                                                                                 : EXIT_FAILURE;
    }
    if (application.arguments().value(1) == QStringLiteral("--single-instance-repeat-request")) {
        const int version = application.arguments().value(4).toInt();
        const auto frame = requestFrame(application.arguments().value(3), version);
        for (int index = 0; index < 2; ++index) {
            QLocalSocket socket;
            socket.connectToServer(application.arguments().value(2));
            require(socket.waitForConnected(1000) && socket.write(frame) == frame.size() &&
                        (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000)) &&
                        (socket.bytesAvailable() > 0 || socket.waitForReadyRead(1000)),
                    "each repeated request receives its dispatch receipt");
        }
        return EXIT_SUCCESS;
    }
    if (application.arguments().value(1) == QStringLiteral("--single-instance-legacy-forward")) {
        QLocalSocket socket;
        socket.connectToServer(application.arguments().value(2), QIODevice::ReadWrite);
        const auto frame = requestFrame({}, 1, application.arguments().mid(3));
        require(socket.waitForConnected(1000) && socket.write(frame) == frame.size() &&
                    (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000)),
                "legacy client writes the original request without expecting a response");
        socket.disconnectFromServer();
        return EXIT_SUCCESS;
    }
    if (application.arguments().value(1) == QStringLiteral("--single-instance-invalid-request")) {
        QLocalSocket socket;
        socket.connectToServer(application.arguments().value(2), QIODevice::ReadWrite);
        const auto frame = requestFrame(application.arguments().value(4),
                                        application.arguments().value(3).toInt());
        require(socket.waitForConnected(1000) && socket.write(frame) == frame.size() &&
                    (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000)),
                "invalid request is delivered to the modern endpoint");
        require(!socket.waitForReadyRead(100) && socket.bytesAvailable() == 0,
                "invalid requests receive no acceptance receipt");
        return EXIT_SUCCESS;
    }
    if (application.arguments().size() == 3 &&
        application.arguments().at(1) == QStringLiteral("--single-instance-stale-owner")) {
        single_instance::SingleInstanceCoordinator owner;
        const auto result = owner.acquireOrForward(application.arguments());
        require(result.outcome == single_instance::SingleInstanceOutcome::Primary,
                "child could not acquire stale-owner fixture");
        writeReadyFile(application.arguments().at(2));
        return QCoreApplication::exec();
    }
    if (application.arguments().size() >= 3 &&
        application.arguments().at(1) == QStringLiteral("--single-instance-forward")) {
        single_instance::SingleInstanceCoordinator secondary;
        const auto result = secondary.acquireOrForward(application.arguments().mid(2));
        if (result.outcome != single_instance::SingleInstanceOutcome::Forwarded ||
            !result.error.isEmpty()) {
            std::cerr << result.error.toStdString() << '\n';
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

#ifdef Q_OS_WIN
    pipeRejectsAnonymousClients();
    forwardingWaitsForSlowPrimary();
    forwardingRequiresReceipt();
    uncertainLegacyWriteIsNotRetried();
#endif
    forwardingToLegacyPrimaryDispatchesOnce();
    firstInstanceAndQueuedForwarding();
    staleOwnerIsRecovered();
    liveUnreachableOwnerIsNotBypassed();
    stoppingPrimaryRetainsLockAndAllowsRetry();
    repeatedRequestReceiptsDoNotRepeatDispatch();
    refusedIntentIsNotDispatchedOrRemembered();
    legacyClientCanForwardToModernPrimary();
    modernEndpointRequiresVersionAndReceiptId();
    largeModernRequestIsFullyWritten();
    disappearingReceiptEndpointDoesNotDowngrade();
    invalidReceiptsDoNotDowngrade();
    customTimeoutWaitsForColdPrimaryReadiness();
    return 0;
}
