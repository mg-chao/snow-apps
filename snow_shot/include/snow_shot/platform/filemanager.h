#ifndef SNOW_SHOT_PLATFORM_FILEMANAGER_H
#define SNOW_SHOT_PLATFORM_FILEMANAGER_H

#include <QString>
#include <QUrl>
#include <functional>

namespace snow_shot::platform {

struct FileManagerLauncher {
    std::function<bool(const QUrl&)> openDirectory;
    // False also covers an unknown handler. The argument is the containing directory.
    std::function<bool(const QString&)> isSystemFileManager;
    std::function<bool(const QString&)> selectFile;
};

// Select files when the default folder handler is Explorer/Finder; otherwise open their parent.
// Directories always open through the default handler. Failed selection falls back to opening.
// Empty callbacks use native operations. No shell command interpreter is used.
[[nodiscard]] bool revealInFileManager(const QString& path,
                                       const FileManagerLauncher& launcher = {});

} // namespace snow_shot::platform

#endif
