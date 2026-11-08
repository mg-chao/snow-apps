#pragma once

#include "snow_shot/diagnostics/diagnostics.h"

#include <QDir>

namespace snow_shot::diagnostics {

struct DiagnosticsPathOptions {
    bool unifiedApplicationData = false;
    QString selectedDirectory;
    QString stableDirectory;
    QString temporaryDirectory;
    QString applicationName;
    QString packageFamily;
};

// Pure path selection keeps startup (before QApplication) and its probes consistent.
inline DiagnosticsOptions diagnosticsOptionsForPaths(const DiagnosticsPathOptions& paths) {
    DiagnosticsOptions options;
    const bool packaged = !paths.packageFamily.isEmpty();
    if ((paths.unifiedApplicationData || packaged) && !QDir::isAbsolutePath(paths.stableDirectory))
        return options;
    const QString logs = QDir(paths.stableDirectory).filePath(QStringLiteral("logs"));
    if (!paths.unifiedApplicationData && !paths.selectedDirectory.isEmpty())
        options.directories.append(QDir(paths.selectedDirectory).filePath(QStringLiteral("logs")));
    options.directories.append(logs);
    options.crashCaptureDirectory = QDir(logs).filePath(QStringLiteral("crashes"));
    if (!paths.unifiedApplicationData &&
        (!packaged || QDir::isAbsolutePath(paths.temporaryDirectory))) {
        const QString relative =
            packaged ? QStringLiteral("SnowShot/%1/%2/logs")
                           .arg(paths.applicationName, paths.packageFamily)
                     : QStringLiteral("SnowShot/%1/logs").arg(paths.applicationName);
        options.directories.append(QDir(paths.temporaryDirectory).filePath(relative));
    }
    options.directories.removeDuplicates();
    return options;
}

} // namespace snow_shot::diagnostics
