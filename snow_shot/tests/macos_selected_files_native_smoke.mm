#include "snow_shot/platform/selectedfiles.h"
#include <QGuiApplication>
#include <QThread>
#import <AppKit/AppKit.h>
#include <future>
#include <iostream>

int main(int argc, char** argv) {
    // Interactive only: the caller supplies the expected current selection.
    if (!qEnvironmentVariableIsSet("SNOW_TEST_FINDER_SELECTION") || argc < 2)
        return 77;
    QGuiApplication app(argc, argv);
    const auto backend = snow_shot::platform::createSelectedFileBackend();
    const auto target = backend->captureTarget();
    const NSInteger clipboard = NSPasteboard.generalPasteboard.changeCount;
    auto future = std::async(std::launch::async, [backend, target] {
        return backend->selectedFiles(target, [] { return false; });
    });
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    const auto result = future.get();
    QStringList actual = result.paths;
    QStringList expected = app.arguments().mid(1);
    actual.sort();
    expected.sort();
    if (result.error != snow_shot::platform::SelectedFileError::None || actual != expected ||
        NSPasteboard.generalPasteboard.changeCount != clipboard) {
        std::cerr << "Finder selection/clipboard check failed; error="
                  << static_cast<int>(result.error) << " selected=" << actual.size()
                  << " expected=" << expected.size() << '\n';
        return 1;
    }
    return 0;
}
