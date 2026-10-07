#include "snow_shot/presentation/screenrecordingfolder.h"

#include "snow_shot/storage/settingsadapters.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace snow_shot::presentation::recording {
namespace {
QStringList defaultRecordingDirectories() {
    QStringList directories;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation}) {
        const QString directory = QStandardPaths::writableLocation(location);
        if (!directory.isEmpty() && !directories.contains(directory, Qt::CaseInsensitive)) {
            directories.push_back(directory);
        }
    }
    return directories;
}
} // namespace

QStringList screenRecordingDirectories() {
    QStringList directories;
    const QString configured =
        QDir::cleanPath(storage::RecordingSettings().videoSaveDirectory().trimmed());
    const QFileInfo configuredInfo(configured);
    if (!configured.isEmpty() && configuredInfo.isDir() && configuredInfo.isWritable()) {
        directories.push_back(configured);
    }
    for (const QString& fallback : defaultRecordingDirectories()) {
        if (!directories.contains(fallback, Qt::CaseInsensitive)) {
            directories.push_back(fallback);
        }
    }
    return directories;
}

QString screenRecordingDirectory() {
    const QStringList directories = screenRecordingDirectories();
    for (const QString& candidate : directories) {
        QDir directory(candidate);
        if ((directory.exists() || directory.mkpath(QStringLiteral("."))) &&
            QFileInfo(directory.absolutePath()).isWritable()) {
            return directory.absolutePath();
        }
    }
    return directories.isEmpty() ? QString() : directories.constFirst();
}

bool openScreenRecordingFolder() {
    const QString path = screenRecordingDirectory();
    return !path.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool revealScreenRecordingFile(const QString& path) {
    const QFileInfo file(path);
    if (path.isEmpty() || !file.isFile())
        return false;
#ifdef Q_OS_WIN
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(
        QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(file.absoluteFilePath())));
    return explorer.startDetached();
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                   {QStringLiteral("-R"), file.absoluteFilePath()});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath()));
#endif
}
} // namespace snow_shot::presentation::recording
