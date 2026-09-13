#include "snow_shot/app/singleinstancecoordinator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

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
    single_instance::SingleInstanceResult forwarded;
    std::atomic<bool> forwardingComplete = false;
    std::thread secondaryThread([&forwardedArguments, &forwarded, &forwardingComplete]() {
        single_instance::SingleInstanceCoordinator secondary;
        forwarded = secondary.acquireOrForward(forwardedArguments);
        forwardingComplete = true;
    });
    require(waitUntil([&forwardingComplete]() { return forwardingComplete.load(); }, 3000),
            "second process did not finish its forwarding attempt");
    secondaryThread.join();
    require(forwarded.outcome == single_instance::SingleInstanceOutcome::Forwarded &&
                forwarded.error.isEmpty(),
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

    firstInstanceAndQueuedForwarding();
    staleOwnerIsRecovered();
    liveUnreachableOwnerIsNotBypassed();
    return 0;
}
