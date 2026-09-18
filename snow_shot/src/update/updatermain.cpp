#include "snow_shot/update/updatecontract.h"
#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/platform/windows/privilegedlocalserver.h"
#include "snow_shot/update/updatetransaction.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <cstdio>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <shellapi.h>
#endif

using namespace snow_shot::update;

namespace {
void pruneCoordinators() {
    const QDir temporary(QDir::tempPath());
    static const QRegularExpression generated(QStringLiteral("^snow-shot-updater-[A-Za-z0-9]{6}$"));
    for (const auto& directory : temporary.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!generated.match(directory.fileName()).hasMatch() || directory.isSymLink() ||
            directory.lastModified().secsTo(QDateTime::currentDateTimeUtc()) <= 86400) {
            continue;
        }
#ifdef Q_OS_WIN
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(
            QDir::toNativeSeparators(directory.absoluteFilePath()).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            continue;
        }
#endif
        const QString executable =
            QDir(directory.absoluteFilePath()).filePath(QStringLiteral("coordinator.exe"));
        if (!QFileInfo(executable).isSymLink() && QFile::remove(executable)) {
            // Non-recursive: an unexpected/user-created file always preserves its directory.
            temporary.rmdir(directory.fileName());
        }
    }
}

QString option(const QStringList& args, const QString& name) {
    const auto i = args.indexOf(name);
    requireUpdate(i >= 0 && i + 1 < args.size(), "Missing updater argument");
    return args[i + 1];
}

QStringList replaceMode(QStringList args, const QString& mode) {
    args[0] = mode;
    return args;
}

QStringList invocation;
void verifyCoordinator(QLocalSocket& socket, const QString& pipe) {
    const bool workerPipe = pipe.endsWith(u"-worker");
    const QString expected = QDir(option(invocation, QStringLiteral("--target")))
                                 .filePath(workerPipe ? QStringLiteral("bin/snow-shot-updater.exe")
                                                      : QStringLiteral("bin/snow_shot.exe"));
    const quint32 parent = workerPipe ? 0 : option(invocation, QStringLiteral("--parent")).toUInt();
    requireUpdate(
        snow_shot::platform::windows::verifyLocalPeer(socket, true, expected, workerPipe, parent),
        "The update coordinator identity could not be verified");
}
void sendLine(const QString& pipe, const QByteArray& line) {
    QLocalSocket socket;
    socket.connectToServer(pipe);
    requireUpdate(socket.waitForConnected(10000), "Could not contact the update coordinator");
    verifyCoordinator(socket, pipe);
    socket.write(line + '\n');
    requireUpdate(socket.waitForBytesWritten(5000), "Could not send updater status");
}

QByteArray exchangeLine(QLocalSocket& socket, const QByteArray& line) {
    const QByteArray frame = line + '\n';
    requireUpdate(socket.write(frame) == frame.size() &&
                      (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(5000)),
                  "Could not send updater status");
    QElapsedTimer deadline;
    deadline.start();
    while (!socket.canReadLine()) {
        const auto remaining = 30000 - deadline.elapsed();
        requireUpdate(socket.bytesAvailable() < 4096 && remaining > 0 &&
                          socket.waitForReadyRead(static_cast<int>(remaining)),
                      "The update handoff was not acknowledged");
    }
    return socket.readLine(4096).trimmed();
}

QByteArray exchangeLine(const QString& pipe, const QByteArray& line) {
    QLocalSocket socket;
    socket.connectToServer(pipe);
    requireUpdate(socket.waitForConnected(10000), "Could not contact the update coordinator");
    verifyCoordinator(socket, pipe);
    return exchangeLine(socket, line);
}

bool writable(const QString& root) {
    QTemporaryFile probe(QDir(root).filePath(QStringLiteral(".snow-shot-permission-XXXXXX")));
    return probe.open();
}

bool launchElevated(const QString& executable, const QStringList& args) {
#ifdef Q_OS_WIN
    QStringList quoted;
    for (QString arg : args) {
        requireUpdate(!arg.contains(u'"'), "Invalid updater command argument");
        // All arguments are file names, identifiers, and switches; avoid trailing quote escapes.
        while (arg.endsWith(u'\\')) {
            arg.chop(1);
        }
        quoted.append(u'"' + arg + u'"');
    }
    const QString command = quoted.join(u' ');
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>(executable.utf16());
    info.lpParameters = reinterpret_cast<LPCWSTR>(command.utf16());
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        return false;
    }
    if (info.hProcess != nullptr) {
        CloseHandle(info.hProcess);
    }
    return true;
#else
    Q_UNUSED(executable);
    Q_UNUSED(args);
    return false;
#endif
}

void validateRegisteredTarget(const QString& root) {
#ifdef Q_OS_WIN
    bool matched = false;
    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        wchar_t buffer[32768]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(hive, L"Software\\Snow Apps\\SnowShot", nullptr,
                         RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, buffer,
                         &size) == ERROR_SUCCESS) {
            matched |= QDir::cleanPath(QString::fromWCharArray(buffer))
                           .compare(QDir::cleanPath(root), Qt::CaseInsensitive) == 0;
        }
    }
    requireUpdate(matched, "Elevation requires a registered Snow Shot installation");
#else
    Q_UNUSED(root);
    throw std::runtime_error("Elevation is unavailable on this platform");
#endif
}

int worker(const QStringList& args) {
    const QString root = option(args, QStringLiteral("--target"));
    validateInstallationRoot(root);
    const QString pipe = option(args, QStringLiteral("--pipe")) + QStringLiteral("-worker");
    const QString parentText = option(args, QStringLiteral("--parent"));
    const bool recovery = args.contains(QStringLiteral("--recovery"));
#ifdef Q_OS_WIN
    const DWORD parentId = parentText.toUInt();
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
    requireUpdate(parent != nullptr, "Could not open the application process");
    struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() {
            CloseHandle(handle);
        }
    } guard{parent};
    wchar_t executable[32768]{};
    DWORD length = 32768;
    requireUpdate(QueryFullProcessImageNameW(parent, 0, executable, &length) &&
                      QDir::fromNativeSeparators(
                          QString::fromWCharArray(executable, static_cast<int>(length)))
                              .compare(QDir(root).filePath(QStringLiteral("bin/snow_shot.exe")),
                                       Qt::CaseInsensitive) == 0,
                  "The update target does not match its application process");
#endif
    UpdateRelease release;
    QString archive;
    struct InputGuard {
        QString& path;
        ~InputGuard() {
            if (!path.isEmpty()) {
                QFile::remove(path);
            }
        }
    } inputGuard{archive};
    if (!recovery) {
        release = verifyRelease(readLimited(option(args, QStringLiteral("--manifest"))));
        const auto record = installationRecord(root);
        const auto& package =
            release.updatePackage(record.value(QStringLiteral("variant")).toString());
        requireUpdate(compareVersions(release.version,
                                      record.value(QStringLiteral("version")).toString()) > 0,
                      "The release is not newer than this installation");
        archive = QDir(root).filePath(QStringLiteral(".snow-shot-update/input-") +
                                      QUuid::createUuid().toString(QUuid::Id128) +
                                      QStringLiteral(".zip"));
        requireUpdate(QFile::copy(option(args, QStringLiteral("--archive")), archive),
                      "Could not stage the verified update package");
        verifyFile(archive, package.size, package.sha256);
    }
    // Authenticate once, before changing the installed helper, and retain this
    // connection through completion. Reconnecting afterwards would compare the
    // old broker with the new on-disk helper and reject a legitimate update.
    // It also races peer PID verification against a short-lived worker's exit.
    QLocalSocket coordinator;
    coordinator.connectToServer(pipe);
    requireUpdate(coordinator.waitForConnected(10000), "Could not contact the update coordinator");
    verifyCoordinator(coordinator, pipe);
    requireUpdate(exchangeLine(coordinator, QByteArrayLiteral("ready")) == "go",
                  "The application cancelled the update handoff");
#ifdef Q_OS_WIN
    requireUpdate(WaitForSingleObject(parent, 45000) == WAIT_OBJECT_0,
                  "The application did not exit; the update was cancelled");
#endif
    QByteArray status = QByteArrayLiteral("success");
    try {
        if (recovery) {
            recoverTransaction(root);
        } else {
            applyTransaction(root, archive, release);
        }
    } catch (const std::exception& error) {
        status = QByteArrayLiteral("failed:") + error.what();
    }
    requireUpdate(exchangeLine(coordinator, status) == "done",
                  "The update handoff was not acknowledged");
    return status == "success" ? 0 : 1;
}

int broker(QCoreApplication& app, const QStringList& args) {
    const QString root = option(args, QStringLiteral("--target"));
    QFile originalHelper(app.applicationFilePath());
    QCryptographicHash originalHash(QCryptographicHash::Sha256);
    requireUpdate(originalHelper.open(QIODevice::ReadOnly) && originalHash.addData(&originalHelper),
                  "Could not verify the running update coordinator");
    const QByteArray workerDigest = originalHash.result();
    originalHelper.close();
    const QString pipe = option(args, QStringLiteral("--pipe"));
    snow_shot::platform::windows::PrivilegedLocalServer server;
    requireUpdate(server.listen(pipe + QStringLiteral("-worker")),
                  "Could not create updater coordinator");
    bool handedOff = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(180000);
    auto finish = [&](const QByteArray& status) {
        if (!handedOff) {
            try {
                sendLine(pipe, status);
            } catch (...) {
            }
        } else {
            const QString result = option(args, QStringLiteral("--result"));
            writeAtomic(result, status);
            if (!transactionPending(root)) {
                QProcess::startDetached(QDir(root).filePath(QStringLiteral("bin/snow_shot.exe")),
                                        {QStringLiteral("--show-main-window")}, root);
            }
        }
        app.exit(status == "success" ? 0 : 1);
    };
    QObject::connect(&timeout, &QTimer::timeout, &app, [&]() {
        if (handedOff) {
            // A slow or stuck worker may still own the transaction. Never launch a partially
            // replaced app on a watchdog deadline; a later manual launch enters recovery.
            writeAtomic(option(args, QStringLiteral("--result")),
                        QByteArrayLiteral("failed:The update helper timed out"));
            app.exit(1);
        } else {
            finish(QByteArrayLiteral("failed:The update helper timed out"));
        }
    });
    QObject::connect(
        &server, &snow_shot::platform::windows::PrivilegedLocalServer::newConnection, &app, [&]() {
            while (auto* socket = server.nextPendingConnection()) {
                if (!snow_shot::platform::windows::verifyLocalPeer(
                        *socket, false,
                        QDir(root).filePath(QStringLiteral("bin/snow-shot-updater.exe")), true, 0,
                        workerDigest)) {
                    socket->disconnectFromServer();
                    socket->deleteLater();
                    continue;
                }
                auto read = [&, socket]() {
                    if (!socket->canReadLine()) {
                        return;
                    }
                    const QByteArray status = socket->readLine(4096).trimmed();
                    if (status == "ready") {
                        try {
                            const QByteArray answer = exchangeLine(pipe, status);
                            socket->write(answer + '\n');
                            socket->flush();
                            handedOff = answer == "go";
                            if (handedOff) {
                                timeout.start(15 * 60 * 1000);
                            } else {
                                finish(QByteArrayLiteral(
                                    "failed:Application cancelled the update handoff"));
                            }
                        } catch (...) {
                            finish(
                                QByteArrayLiteral("failed:Application coordinator disconnected"));
                        }
                    } else {
                        if (handedOff) {
                            // Acknowledge the final status on the authenticated
                            // session before either side tears down its process.
                            socket->write("done\n");
                            socket->flush();
                        }
                        finish(status);
                    }
                };
                QObject::connect(socket, &QLocalSocket::readyRead, &app, read);
                QObject::connect(socket, &QLocalSocket::disconnected, socket,
                                 &QObject::deleteLater);
                read();
            }
        });
    const QString original = QDir(root).filePath(QStringLiteral("bin/snow-shot-updater.exe"));
    requireUpdate(
        QProcess::startDetached(original, replaceMode(args, QStringLiteral("--bootstrap"))),
        "Could not launch the installed update helper");
    return app.exec();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    invocation = args;
    try {
        requireUpdate(!args.isEmpty(), "Missing updater operation");
        if (args.first() == u"--verify-release") {
            verifyRelease(readLimited(option(args, QStringLiteral("--manifest"))));
            return 0;
        }
        if (args.first() == u"--audit-release") {
            auditRelease(option(args, QStringLiteral("--directory")),
                         verifyRelease(readLimited(option(args, QStringLiteral("--manifest")))));
            return 0;
        }
        const QString root = QDir::cleanPath(option(args, QStringLiteral("--target")));
        if (args.first() == u"--migrate-startup") {
            validateInstallationRoot(root);
            const QString previous = option(args, QStringLiteral("--previous"));
            requireUpdate(QDir::isAbsolutePath(previous), "Invalid previous installation path");
            const auto migrated =
                snow_shot::platform::windows::migrateInstallationStartup(previous, root);
            requireUpdate(migrated.success, migrated.error.toUtf8().constData());
            return 0;
        }
        if (args.first() == u"--launch-desktop") {
            validateInstallationRoot(root);
            return snow_shot::platform::windows::launchOnInteractiveDesktop(
                       QDir(root).filePath(QStringLiteral("bin/snow_shot.exe")))
                       ? 0
                       : 1;
        }
        if (args.first() == u"--uninstall") {
            validateInstallationRoot(root);
            if (!args.contains(QStringLiteral("--upgrade"))) {
                const auto cleanup = snow_shot::platform::windows::removeInstallationStartup(root);
                requireUpdate(cleanup.success, cleanup.error.toUtf8().constData());
            }
            uninstallOwnedFiles(root);
            return 0;
        }
        if (args.first() == u"--launch") {
            pruneCoordinators();
            QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/snow-shot-updater-XXXXXX"));
            requireUpdate(temporary.isValid(), "Could not create updater coordinator directory");
            const QString executable = temporary.filePath(QStringLiteral("coordinator.exe"));
            requireUpdate(QFile::copy(app.applicationFilePath(), executable),
                          "Could not copy updater coordinator");
            requireUpdate(
                QProcess::startDetached(executable, replaceMode(args, QStringLiteral("--broker"))),
                "Could not launch updater coordinator");
            temporary.setAutoRemove(false);
            return 0;
        }
        if (args.first() == u"--broker") {
            return broker(app, args);
        }
        if (args.first() == u"--bootstrap" || args.first() == u"--elevated") {
            validateInstallationRoot(root);
            installationRecord(root);
            validateTargetPath(root, QStringLiteral("bin/snow-shot-updater.exe"));
            if (args.first() == u"--elevated") {
                validateRegisteredTarget(root);
            }
            if (!writable(root)) {
                validateRegisteredTarget(root);
                requireUpdate(args.first() != u"--elevated" &&
                                  launchElevated(app.applicationFilePath(),
                                                 replaceMode(args, QStringLiteral("--elevated"))),
                              "Update permission was declined or could not be obtained");
                return 0;
            }
            const QString directory = QDir(root).filePath(QStringLiteral(".snow-shot-update"));
            requireUpdate(QDir().mkpath(directory), "Could not create update worker directory");
            pruneUpdateWork(root);
            // Unique worker path stays outside the payload and inherits installation permissions.
            const QString executable = QDir(directory).filePath(
                QStringLiteral("worker-") + QUuid::createUuid().toString(QUuid::Id128) +
                QStringLiteral(".exe"));
            requireUpdate(QFile::copy(app.applicationFilePath(), executable) &&
                              QProcess::startDetached(
                                  executable, replaceMode(args, QStringLiteral("--worker"))),
                          "Could not launch update worker");
            return 0;
        }
        if (args.first() == u"--worker") {
            return worker(args);
        }
        throw std::runtime_error("Unknown updater operation");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        if (args.contains(QStringLiteral("--pipe"))) {
            try {
                const QString suffix =
                    args.first() == u"--launch" ? QString() : QStringLiteral("-worker");
                sendLine(option(args, QStringLiteral("--pipe")) + suffix,
                         QByteArrayLiteral("failed:") + error.what());
            } catch (...) {
            }
        }
        return 1;
    }
}
