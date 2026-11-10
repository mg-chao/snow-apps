#include "snow_shot/platform/filemanager.h"
#include "platform/filemanagerbackend_p.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#ifdef Q_OS_WIN
void fileManagerWindowsTests();
#endif

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
    if (argc == 3 && application.arguments()[1] == QStringLiteral("--native-handler")) {
        const bool system = snow_shot::platform::detail::isSystemFileManager(
            QFileInfo(application.arguments()[2]).absoluteFilePath());
        std::cout << (system ? "System file manager\n" : "Custom or unknown file manager\n");
        return 0;
    }
#ifdef Q_OS_WIN
    fileManagerWindowsTests();
#endif
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
    int queryCalls = 0;
    int selectionCalls = 0;
    bool systemManager = false;
    bool selectionSucceeds = true;
    bool launchSucceeds = true;
    snow_shot::platform::FileManagerLauncher launcher;
    launcher.isSystemFileManager = [&](const QString& directoryPath) {
        ++queryCalls;
        require(directoryPath == parentPath, "query the containing directory's default handler");
        return systemManager;
    };
    launcher.selectFile = [&](const QString& filePath) {
        ++selectionCalls;
        require(filePath == path, "select the absolute Unicode file path without shell parsing");
        return selectionSucceeds;
    };
    launcher.openDirectory = [&](const QUrl& url) {
        ++directoryCalls;
        require(url.isLocalFile() && url.toLocalFile() == parentPath,
                "directories are opened as correctly encoded local-file URLs");
        return launchSucceeds;
    };
    using snow_shot::platform::revealInFileManager;
    require(revealInFileManager(path, launcher), "reveal an existing file");
    require(queryCalls == 1 && selectionCalls == 0 && directoryCalls == 1,
            "a custom or unknown handler opens the directory without forcing Explorer/Finder");
    require(revealInFileManager(parentPath, launcher), "open a directory's contents");
    require(queryCalls == 1 && selectionCalls == 0 && directoryCalls == 2,
            "opening a directory skips both handler detection and file selection");
    const QString previousDirectory = QDir::currentPath();
    require(QDir::setCurrent(parentPath), "enter the file-manager fixture directory");
    require(revealInFileManager(fileName, launcher), "resolve relative files before opening");
    require(QDir::setCurrent(previousDirectory), "restore the working directory");
    require(queryCalls == 2 && selectionCalls == 0 && directoryCalls == 3,
            "relative files also use the default directory handler");
    systemManager = true;
    require(revealInFileManager(path, launcher), "select a file in the default system manager");
    require(queryCalls == 3 && selectionCalls == 1 && directoryCalls == 3,
            "successful native selection does not also open the directory");
    systemManager = false;
    require(revealInFileManager(path, launcher), "honor changes to the default file manager");
    require(queryCalls == 4 && selectionCalls == 1 && directoryCalls == 4,
            "handler detection is refreshed on each file reveal");
    systemManager = true;
    selectionSucceeds = false;
    require(revealInFileManager(path, launcher), "fall back to opening when selection fails");
    require(queryCalls == 5 && selectionCalls == 2 && directoryCalls == 5,
            "failed native selection opens the parent exactly once");
    launchSucceeds = false;
    require(!revealInFileManager(path, launcher) && !revealInFileManager(parentPath, launcher),
            "propagate native launcher failures");
    require(queryCalls == 6 && selectionCalls == 3 && directoryCalls == 7,
            "selection and directory launch failures are both propagated");
    const int previousCalls = queryCalls + selectionCalls + directoryCalls;
    require(!revealInFileManager({}, launcher) &&
                !revealInFileManager(directory.filePath(QStringLiteral("missing")), launcher),
            "empty and missing targets fail");
    require(queryCalls + selectionCalls + directoryCalls == previousCalls,
            "invalid targets never query handlers or launch anything");
    std::cout << "File-manager tests passed\n";
}
