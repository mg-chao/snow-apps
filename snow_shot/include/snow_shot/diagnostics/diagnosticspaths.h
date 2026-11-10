#pragma once

#include "snow_shot/diagnostics/diagnostics.h"

#include <QDir>

namespace snow_shot::diagnostics {

struct DiagnosticsPathOptions {
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
    if (packaged && !QDir::isAbsolutePath(paths.stableDirectory))
        return options;
    if (QDir::isAbsolutePath(paths.selectedDirectory))
        options.directories.append(QDir(paths.selectedDirectory).filePath(QStringLiteral("logs")));
    if (QDir::isAbsolutePath(paths.stableDirectory))
        options.directories.append(QDir(paths.stableDirectory).filePath(QStringLiteral("logs")));
    // Standalone capture defaults to the log root that actually passes the writable probe,
    // including portable/custom storage and fallback locations. Store capture stays package-owned.
    if (packaged)
        options.crashCaptureDirectory =
            QDir(paths.stableDirectory).filePath(QStringLiteral("logs/crashes"));
    if (QDir::isAbsolutePath(paths.temporaryDirectory)) {
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
