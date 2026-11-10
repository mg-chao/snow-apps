#include "snow_shot/platform/filemanager.h"
#include "filemanagerbackend_p.h"

#include <QDesktopServices>
#include <QFileInfo>

namespace snow_shot::platform {

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
bool detail::isSystemFileManager(const QString&) {
    return false;
}

bool detail::selectInSystemFileManager(const QString&) {
    return false;
}
#endif

bool revealInFileManager(const QString& path, const FileManagerLauncher& launcher) {
    const QFileInfo file(path);
    if (path.isEmpty() || (!file.isFile() && !file.isDir()))
        return false;
    const QString directory = file.isDir() ? file.absoluteFilePath() : file.absolutePath();
    if (file.isFile()) {
        // Query on demand so changing the default manager takes effect immediately.
        const bool systemManager = launcher.isSystemFileManager
                                       ? launcher.isSystemFileManager(directory)
                                       : detail::isSystemFileManager(directory);
        if (systemManager) {
            const QString absolutePath = file.absoluteFilePath();
            const bool selected = launcher.selectFile
                                      ? launcher.selectFile(absolutePath)
                                      : detail::selectInSystemFileManager(absolutePath);
            if (selected)
                return true;
        }
    }
    const QUrl url = QUrl::fromLocalFile(directory);
    return launcher.openDirectory ? launcher.openDirectory(url) : QDesktopServices::openUrl(url);
}

} // namespace snow_shot::platform
