#ifndef SNOW_SHOT_PLATFORM_FILEMANAGER_H
#define SNOW_SHOT_PLATFORM_FILEMANAGER_H

#include <QString>
#include <QUrl>
#include <functional>

namespace snow_shot::platform {

struct FileManagerLauncher {
    std::function<bool(const QUrl&)> openDirectory;
};

// Open a file's containing directory in the default file manager, or a directory's contents.
// An empty callback uses the native launcher. No shell command interpreter is used.
[[nodiscard]] bool revealInFileManager(const QString& path,
                                       const FileManagerLauncher& launcher = {});

} // namespace snow_shot::platform

#endif
