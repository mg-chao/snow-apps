#include "snow_shot/platform/filemanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    // Opt-in native smoke check; the default test never launches a file manager.
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--native-reveal")) {
        QGuiApplication application(argc, argv);
        return snow_shot::platform::revealInFileManager(application.arguments()[2]) ? 0 : 1;
    }
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "create file-manager fixture directory");
    const QString parentPath = directory.filePath(QString::fromUtf8("history 测试, & $(test) #1"));
    require(QDir().mkpath(parentPath), "create file-manager fixture parent directory");
    const QString fileName = QString::fromUtf8("capture 测试, & $(test) #1.png");
    const QString path = QDir(parentPath).filePath(fileName);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create file-manager fixture file");
    file.close();
    int directoryCalls = 0;
    bool launchSucceeds = true;
    snow_shot::platform::FileManagerLauncher launcher;
    launcher.openDirectory = [&](const QUrl& url) {
        ++directoryCalls;
        require(url.isLocalFile() && url.toLocalFile() == parentPath,
                "directories are opened as correctly encoded local-file URLs");
        return launchSucceeds;
    };
    using snow_shot::platform::revealInFileManager;
    require(revealInFileManager(path, launcher), "reveal an existing file");
    require(directoryCalls == 1,
            "open the containing directory through the system's default folder handler");
    require(revealInFileManager(parentPath, launcher), "open a directory's contents");
    const QString previousDirectory = QDir::currentPath();
    require(QDir::setCurrent(parentPath), "enter the file-manager fixture directory");
    require(revealInFileManager(fileName, launcher), "resolve relative files before opening");
    require(QDir::setCurrent(previousDirectory), "restore the working directory");
    launchSucceeds = false;
    require(!revealInFileManager(path, launcher) && !revealInFileManager(parentPath, launcher),
            "propagate native launcher failures");
    require(directoryCalls == 5,
            "each valid target uses the default folder handler exactly once, even on failure");
    const int previousCalls = directoryCalls;
    require(!revealInFileManager({}, launcher) &&
                !revealInFileManager(directory.filePath(QStringLiteral("missing")), launcher),
            "empty and missing targets fail");
    require(directoryCalls == previousCalls, "invalid targets never launch anything");
    std::cout << "File-manager tests passed\n";
}
