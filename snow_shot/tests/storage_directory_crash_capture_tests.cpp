#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/storagedirectorychange.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <stdexcept>

namespace {
namespace storage = snow_shot::storage;
namespace diagnostics = snow_shot::diagnostics;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void migrate(const QString& destination, bool existingData) {
    auto& app = storage::ApplicationStorage::instance();
    bool finished = false;
    storage::StorageDirectoryChangeResult outcome;
    const auto connection = QObject::connect(
        &app, &storage::ApplicationStorage::directoryChangeFinished, &app, [&](const auto& result) {
            outcome = result;
            finished = true;
        });
    require(app.requestDirectoryChange(destination, existingData).success, "migration starts");
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    QObject::disconnect(connection);
    require(finished && outcome.success, "migration with crash capture completes");
    require(app.configurationDirectory() == destination && !app.directoryChanging(),
            "storage switches without terminating the application");
}

int archiveFixture(const QString& root) {
    diagnostics::DiagnosticsService logger;
    diagnostics::DiagnosticsOptions options;
    options.directories = {QDir(root).filePath(QStringLiteral("source/logs"))};
    options.handlerPath = QStringLiteral(SNOW_TEST_CRASHPAD_HANDLER);
    options.mirrorToConsole = false;
    require(logger.initialize(options) && logger.status().crashCaptureAvailable,
            "archive fixture starts crash capture");
    qFatal("migration.legacy_crash");
    return 3;
}

int fixture(const QString& root, bool stableDatabase) {
    const QString source = QDir(root).filePath(QStringLiteral("source"));
    auto& app = storage::ApplicationStorage::instance();
    require(app.initialize({QDir(root).filePath(QStringLiteral("bin")), source, 60000}).success,
            "fixture storage initializes");
    auto& logger = diagnostics::DiagnosticsService::instance();
    diagnostics::DiagnosticsOptions options;
    options.directories = {QDir(source).filePath(QStringLiteral("logs"))};
    options.handlerPath = QStringLiteral(SNOW_TEST_CRASHPAD_HANDLER);
    if (stableDatabase)
        options.crashCaptureDirectory = QDir(root).filePath(QStringLiteral("runtime/crashes"));
    options.mirrorToConsole = false;
    require(logger.initialize(options) && logger.status().crashCaptureAvailable,
            "native crash capture starts");
    const QString pipe = logger.crashPipeName();
    const QString database = logger.crashCaptureDirectory();
    require(!pipe.isEmpty(), "capture endpoint is available");

    migrate(QDir(root).filePath(QStringLiteral("first")), true);
    require(logger.status().crashCaptureAvailable && logger.crashPipeName() == pipe,
            "migration preserves crash capture and its endpoint");
    require(logger.crashCaptureDirectory() == database && QFileInfo(database).isDir(),
            "migration preserves the live database, including when it is inside the source");
    if (stableDatabase) {
        const auto exported = logger.exportDay(QDate::currentDate()).get();
        QFile snapshot(exported.path);
        require(exported.success && snapshot.open(QIODevice::ReadOnly) &&
                    snapshot.readAll().contains("crash.summary"),
                "migrated legacy crash reports remain exportable");
    }
    migrate(QDir(root).filePath(QStringLiteral("second")), false);
    require(logger.status().crashCaptureAvailable && logger.crashPipeName() == pipe,
            "repeated directory changes preserve crash capture");
    require(logger.crashCaptureDirectory() == database && logger.directories().contains(database),
            "capture storage remains visible to retention and disk usage");
    qWarning("migration.crash_capture_verified");
    require(logger.flush(), "logging continues after migration");
    QFile log(logger.status().currentFile);
    require(log.open(QIODevice::ReadOnly) &&
                log.readAll().contains("migration.crash_capture_verified"),
            "Qt messages reach the new log directory");
    QFile completed(QDir(root).filePath(QStringLiteral("completed")));
    const QByteArray session = logger.status().sessionId.toUtf8();
    require(completed.open(QIODevice::WriteOnly) && completed.write(session) == session.size(),
            "publish completed migrations before the intentional crash");
    completed.close();
    qFatal("migration.intentional_crash");
    return 3;
}

QVector<diagnostics::CrashReport> reports(const QString& directory) {
    auto collector = diagnostics::makeCrashCollector();
    QString error;
    require(collector->initialize(directory, {}, {}, &error), "open fixture crash database");
    QElapsedTimer timer;
    timer.start();
    auto result = collector->reports();
    while (result.isEmpty() && timer.elapsed() < 5000) {
        QThread::msleep(25);
        result = collector->reports();
    }
    return result;
}

void verifyMigration(bool stableDatabase) {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary migration root");
    if (stableDatabase) {
        QProcess archive;
        archive.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--archive-fixture"), directory.path()});
        require(archive.waitForStarted(10000) && archive.waitForFinished(15000),
                "legacy crash fixture finishes");
        require(reports(directory.filePath(QStringLiteral("source/logs/crashes"))).size() == 1,
                "legacy database contains a report before migration");
    }
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--fixture"), directory.path(),
                 stableDatabase ? QStringLiteral("stable") : QStringLiteral("nested")});
    require(child.waitForStarted(10000) && child.waitForFinished(60000),
            "native migration fixture finishes");
    if (!QFileInfo::exists(directory.filePath(QStringLiteral("completed")))) {
        std::fprintf(stderr, "%s", child.readAllStandardError().constData());
    }
    require(QFileInfo::exists(directory.filePath(QStringLiteral("completed"))),
            "application survives both migrations with native crash capture enabled");
    require(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0,
            "fixture reaches its intentional crash");
    const auto captured =
        reports(directory.filePath(stableDatabase ? QStringLiteral("runtime/crashes")
                                                  : QStringLiteral("source/logs/crashes")));
    require(captured.size() == 1, "crash capture still writes exactly one report after migrations");
    QFile dump(captured.front().path);
    require(dump.open(QIODevice::ReadOnly), "post-migration crash dump is readable");
    const QByteArray bytes = dump.readAll();
    QFile completed(directory.filePath(QStringLiteral("completed")));
    require(completed.open(QIODevice::ReadOnly), "completed session identity is readable");
    require(bytes.startsWith("MDMP") && bytes.contains("migration.crash_capture_verified") &&
                bytes.contains(completed.readAll()),
            "post-migration dump retains the current session and breadcrumbs");
    diagnostics::DiagnosticsOptions recoveryOptions;
    recoveryOptions.directories = {directory.filePath(QStringLiteral("second/logs"))};
    recoveryOptions.crashCaptureDirectory = directory.filePath(
        stableDatabase ? QStringLiteral("runtime/crashes") : QStringLiteral("source/logs/crashes"));
    recoveryOptions.enableCrashCapture = false;
    recoveryOptions.installMessageHandler = false;
    diagnostics::DiagnosticsService recovery;
    require(recovery.initialize(recoveryOptions), "recovery logger starts without native capture");
    const auto exported = recovery.exportDay(QDate::currentDate()).get();
    QFile snapshot(exported.path);
    require(exported.success && snapshot.open(QIODevice::ReadOnly) &&
                snapshot.readAll().contains("crash.summary"),
            "stable capture database remains included in crash-day export");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().value(1) == u"--fixture")
            return fixture(app.arguments().at(2), app.arguments().value(3) == u"stable");
        if (app.arguments().value(1) == u"--archive-fixture")
            return archiveFixture(app.arguments().at(2));
        verifyMigration(false);
        verifyMigration(true);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
