// Measures the real application's process-launch-to-IPC-delivery path. The primary is an
// isolated coordinator, so this benchmark never opens the user's files or changes registrations.
#include "snow_shot/app/edition.h"
#include "snow_shot/app/shellimagelaunch.h"
#include "snow_shot/app/singleinstancecoordinator.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("app"), QStringLiteral("Release application executable"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON report"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured process launches"),
                      QStringLiteral("count"), QStringLiteral("7")});
    parser.addOption(
        {QStringLiteral("offscreen"), QStringLiteral("Use the offscreen Qt platform")});
    parser.addOption({QStringLiteral("require-forward-only"),
                      QStringLiteral("Require forwarding without storage or startup helpers")});
    parser.process(application);
    try {
        const QFileInfo source(parser.value(QStringLiteral("app")));
        const QFileInfo report(parser.value(QStringLiteral("output")));
        bool validCount = false;
        const int count = parser.value(QStringLiteral("samples")).toInt(&validCount);
        require(source.isFile() && !parser.value(QStringLiteral("output")).isEmpty(),
                "--app and --output are required");
        require(validCount && count >= 1 && count <= 100, "--samples must be between 1 and 100");
        require(QDir().mkpath(report.absolutePath()), "create report directory");
        QTemporaryDir isolated(
            QDir(report.absolutePath()).filePath(QStringLiteral("launcher-XXXXXX")));
        require(isolated.isValid(), "create isolated launch directory");
        const QDir staged(isolated.filePath(QStringLiteral("bin")));
        require(QDir().mkpath(staged.path()), "create isolated installation version directory");
        const auto executable = staged.filePath(source.fileName());
        require(QFile::copy(source.absoluteFilePath(), executable), "stage application executable");
        const bool forwardOnly = parser.isSet(QStringLiteral("require-forward-only"));
        if (!forwardOnly) {
            const auto handler = QStringLiteral("crashpad_handler.exe");
            require(QFile::copy(source.dir().filePath(handler), staged.filePath(handler)),
                    "stage crash handler for representative pre-forward initialization");
            const auto updater = snow_shot::app::edition::updaterName();
            require(QFile::copy(source.dir().filePath(updater), staged.filePath(updater)),
                    "stage updater for representative transaction-state check");
        }
        QFile marker(staged.filePath(snow_shot::app::edition::portableMarkerName()));
        require(marker.open(QIODevice::WriteOnly) && marker.write("data") == 4,
                "isolate portable diagnostics and configuration");
        marker.close();
        const auto instance = QUuid::createUuid().toString(QUuid::Id128);
        QCoreApplication::setOrganizationName(snow_shot::app::edition::registryName());
        QCoreApplication::setApplicationName(snow_shot::app::edition::applicationName() +
                                             QStringLiteral("-e2e-") + instance);
        snow_shot::app::SingleInstanceCoordinator primary;
        require(primary.acquireOrForward({}).outcome ==
                    snow_shot::app::SingleInstanceOutcome::Primary,
                "isolated primary owns the instance");
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PATH"), source.absolutePath() + QDir::listSeparator() +
                                                       environment.value(QStringLiteral("PATH")));
        const auto imagePath = isolated.filePath(QString::fromUtf8("selected-雪.png"));
        QStringList arguments{QStringLiteral("--e2e-instance-id=") + instance};
        arguments.append(snow_shot::app::shellImageLaunchArguments({imagePath}));
        if (parser.isSet(QStringLiteral("offscreen")))
            arguments.append({QStringLiteral("-platform"), QStringLiteral("offscreen")});
        QElapsedTimer timer;
        qint64 deliveredNs = -1;
        int deliveries = 0;
        primary.setLaunchRequestHandler([&](const QStringList& forwarded) {
            const auto request = snow_shot::app::parseShellImageLaunchRequest(forwarded);
            require(request.valid && request.paths == QStringList{imagePath},
                    "forwarded shell request retains its Unicode path");
            deliveredNs = timer.nsecsElapsed();
            ++deliveries;
        });
        QJsonArray samples;
        std::vector<double> deliveryTimes;
        for (int index = 0; index < count; ++index) {
            QProcess child;
            child.setProcessEnvironment(environment);
            child.setWorkingDirectory(isolated.path());
            deliveredNs = -1;
            deliveries = 0;
            timer.start();
            child.start(executable, arguments);
            require(child.waitForStarted(10000), "start application forwarding process");
            while (child.state() != QProcess::NotRunning && timer.elapsed() < 30000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                child.waitForFinished(5000);
                throw std::runtime_error("application forwarding timed out");
            }
            const double exitMs = double(timer.nsecsElapsed()) / 1'000'000.0;
            if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0)
                std::cerr << child.readAllStandardError().constData();
            require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 &&
                        deliveredNs >= 0 && deliveries == 1,
                    "application forwards exactly once and exits successfully");
            const double deliveryMs = double(deliveredNs) / 1'000'000.0;
            deliveryTimes.push_back(deliveryMs);
            samples.append(QJsonObject{{QStringLiteral("delivery_ms"), deliveryMs},
                                       {QStringLiteral("exit_ms"), exitMs}});
        }
        const bool diagnosticsCreated = QDir(staged.filePath(QStringLiteral("data/logs"))).exists();
        require(!forwardOnly || !QDir(staged.filePath(QStringLiteral("data"))).exists(),
                "a forwarding-only process must not probe storage or initialize diagnostics");
        std::sort(deliveryTimes.begin(), deliveryTimes.end());
        const QJsonObject result{
            {QStringLiteral("scenario"), QStringLiteral("shell-launch-to-running-primary")},
            {QStringLiteral("samples"), samples},
            {QStringLiteral("median_delivery_ms"), deliveryTimes[deliveryTimes.size() / 2]},
            {QStringLiteral("diagnostics_created"), diagnosticsCreated}};
        QFile output(report.absoluteFilePath());
        require(output.open(QIODevice::WriteOnly) &&
                    output.write(QJsonDocument(result).toJson()) > 0,
                "write performance report");
        std::cout << QJsonDocument(result).toJson().constData();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
