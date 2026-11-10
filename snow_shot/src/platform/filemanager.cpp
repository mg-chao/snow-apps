#include "snow_shot/platform/filemanager.h"

#include <QDesktopServices>
#include <QFileInfo>

namespace snow_shot::platform {

bool revealInFileManager(const QString& path, const FileManagerLauncher& launcher) {
    const QFileInfo file(path);
    if (path.isEmpty() || (!file.isFile() && !file.isDir()))
        return false;
    // Open the folder URL through the system's default handler on every platform.
    // Revealing a file via Explorer or Finder would bypass a custom file manager.
    const QString directory = file.isDir() ? file.absoluteFilePath() : file.absolutePath();
    const QUrl url = QUrl::fromLocalFile(directory);
    return launcher.openDirectory ? launcher.openDirectory(url) : QDesktopServices::openUrl(url);
}

} // namespace snow_shot::platform
