#include "snow_shot/app/singleinstancecoordinator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QLocalServer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <iostream>

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

#ifdef Q_OS_WIN
class DelayedAcceptServer final : public QLocalServer {
  protected:
    void incomingConnection(quintptr descriptor) override {
        // Keep the client's write pending longer than Qt's 10 ms pipe poll.
        QTimer::singleShot(100, this,
                           [this, descriptor] { QLocalServer::incomingConnection(descriptor); });
    }
};

void forwardingWaitsForSlowPrimary() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the slow-primary fixture");
    auto* originalServer = primary.findChild<QLocalServer*>();
    require(originalServer != nullptr, "primary has no IPC server");
    const auto name = originalServer->serverName();
    originalServer->close();
    DelayedAcceptServer delayed;
    delayed.setSocketOptions(QLocalServer::UserAccessOption);
    require(delayed.listen(name), "could not listen as the slow primary");
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

void pipeRejectsAnonymousClients() {
    single_instance::SingleInstanceCoordinator primary;
    require(primary.acquireOrForward({QStringLiteral("snow-shot-test")}).outcome ==
                single_instance::SingleInstanceOutcome::Primary,
            "could not acquire the pipe-permissions fixture");
    const auto* server = primary.findChild<QLocalServer*>();
    require(server != nullptr, "primary has no IPC server");
    const auto name = server->fullServerName().toStdWString();
    // Unlike the default named-pipe ACL, our same-user endpoint must not grant
    // anonymous/Everyone access. This also guards against fixing elevation by
    // making the single-instance pipe world-accessible.
    require(ImpersonateAnonymousToken(GetCurrentThread()),
            "could not impersonate an anonymous pipe client");
    const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
    const DWORD error = GetLastError();
    const BOOL reverted = RevertToSelf();
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
    require(reverted, "could not restore the test thread token");
    require(pipe == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
            "single-instance pipe allowed an anonymous client");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("single-instance-coordinator"));
    QCoreApplication application(argc, argv);
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
#endif
    firstInstanceAndQueuedForwarding();
    staleOwnerIsRecovered();
    liveUnreachableOwnerIsNotBypassed();
    return 0;
}
