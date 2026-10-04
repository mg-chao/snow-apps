#include "snow_shot/update/startupupdate.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

namespace snow_shot::update {
namespace {
constexpr auto kSkipStartupUpdate = "--skip-startup-update";
}

UpdateService::Options defaultUpdateServiceOptions() {
    UpdateService::Options options;
    options.applicationDirectory = QCoreApplication::applicationDirPath();
    options.root = QFileInfo(options.applicationDirectory).dir().absolutePath();
    const QString identity = QString::fromLatin1(
        QCryptographicHash::hash(options.root.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(24));
    options.cacheDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                                 .filePath(QStringLiteral("updates/") + identity);
    return options;
}

bool isStartupUpdateRelaunch(const QStringList& arguments) {
    return arguments.contains(QString::fromLatin1(kSkipStartupUpdate));
}

QStringList normalStartupArguments(QStringList arguments) {
    arguments.removeAll(QString::fromLatin1(kSkipStartupUpdate));
    return arguments;
}

QStringList startupUpdateRelaunchArguments(bool foregroundRequested) {
    return {foregroundRequested ? QStringLiteral("--show-main-window")
                                : QStringLiteral("--autostart"),
            QString::fromLatin1(kSkipStartupUpdate)};
}

StartupUpdateResult runStartupUpdate(UpdateService& service, const std::function<bool()>& flush,
                                     const std::function<bool()>& foregroundRequested,
                                     std::chrono::milliseconds timeout) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool finished = false;
    StartupUpdateResult result = StartupUpdateResult::ContinueStartup;
    const auto finish = [&] {
        finished = true;
        deadline.stop();
        loop.quit();
    };
    QObject::connect(&service, &UpdateService::operationFinished, &loop,
                     [&](const QString&, const QString&) { finish(); });
    QObject::connect(&service, &UpdateService::handoffReady, &loop, [&] {
        if (!flush || !flush()) {
            service.reportBlocked(QCoreApplication::translate(
                "snow_shot::app::ApplicationController",
                "Your settings could not be saved. Please retry before updating."));
            return;
        }
        service.setRelaunchArguments(
            startupUpdateRelaunchArguments(foregroundRequested && foregroundRequested()));
    });
    QObject::connect(&service, &UpdateService::handoffCommitted, &loop, [&] {
        result = StartupUpdateResult::ExitForUpdate;
        finish();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        service.cancel();
        finish();
    });
    deadline.start(timeout);
    service.applyAtStartup();
    if (!finished)
        loop.exec();
    return result;
}

} // namespace snow_shot::update
