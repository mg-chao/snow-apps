#include "snow_shot/presentation/windowgroupswitcherpopup.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include <QTemporaryDir>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QFontDatabase>
#include <QDir>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include <iostream>
using namespace snow_shot::presentation;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path()}).success)
        return 1;
    styles::ThemeManager::instance().initialize(app);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        for (const QString& file :
             {QStringLiteral("segoeui.ttf"), QStringLiteral("segoeuib.ttf")}) {
            if (QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR"))
                                                      .filePath(QStringLiteral("Fonts/") + file)) <
                0)
                return 1;
        }
        styles::ThemeStyleConfig config;
        config.appFont = QFont(QStringLiteral("Segoe UI"));
        styles::ThemeManager::instance().setThemeStyleConfig(config);
    }
#endif
    QVector<WindowGroupDisplayEntry> entries;
    for (int i = 0; i < 128; ++i)
        entries.append(
            {QString::number(i), QStringLiteral("Window group %1").arg(i), {i * 3, i * 4}});
    QElapsedTimer timer;
    timer.start();
    WindowGroupSwitcherPopup popup;
    popup.setGroups(entries, QStringLiteral("0"));
    popup.setSelectedGroup(QStringLiteral("1"));
    popup.setShortcutMode(true);
    popup.showOnScreen(app.primaryScreen());
    app.processEvents();
    auto pixels = popup.grab();
    const double cold = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    QVector<double> warm;
    QVector<double> cycle;
    for (int i = 0; i < 200; ++i) {
        popup.hide();
        timer.restart();
        popup.setGroups(entries, QStringLiteral("0"));
        popup.showOnScreen(app.primaryScreen());
        popup.setSelectedGroup(QString::number(i % 128));
        app.processEvents();
        pixels = popup.grab();
        warm.append(static_cast<double>(timer.nsecsElapsed()) / 1e6);
        timer.restart();
        popup.setSelectedGroup(QString::number((i + 1) % 128));
        app.processEvents();
        pixels = popup.grab();
        cycle.append(static_cast<double>(timer.nsecsElapsed()) / 1e6);
    }
    std::sort(warm.begin(), warm.end());
    std::sort(cycle.begin(), cycle.end());
    std::cout << "128 groups; cold open + paint: " << cold << " ms; warm p50/p95: " << warm[100]
              << "/" << warm[190] << " ms; cycle + paint p50/p95: " << cycle[100] << "/"
              << cycle[190] << " ms\n";
    storage.shutdown();
    return pixels.isNull() ? 1 : 0;
}
