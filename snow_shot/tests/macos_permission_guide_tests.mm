#include "snow_shot/presentation/permissionguidecontroller.h"
#include "snow_shot/presentation/components/permissionguidewidget.h"
#import <AppKit/AppKit.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMimeData>
#include <QTimer>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::presentation;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class ProbePermissions final : public AppPermissionBackend {
    AppPermissionSnapshot query() override {
        AppPermissionSnapshot result;
        result.statuses.fill(AppPermissionStatus::Missing);
        return result;
    }
    void request(AppPermission, std::function<void()> done) override {
        done();
    }
    bool openSettings(AppPermission permission) override {
        return createAppPermissionBackend()->openSettings(permission);
    }
};
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    // Model the real workflow: the app already has a settings window before
    // the user opens System Settings. Qt activates on its first visible window.
    QWidget host;
    host.setWindowTitle(QStringLiteral("Snow Shot Permission Guide Test"));
    host.resize(320, 160);
    host.show();
    QApplication::processEvents();
    auto platform = createPermissionGuidePlatform();
    const auto identity = platform->application();
    require(identity.bundleUrl.isLocalFile(), "native fixture must run from its .app bundle");
    require(createAppPermissionBackend()->openSettings(AppPermission::ScreenRecording),
            "native fixture opens System Settings without requesting access");
    QEventLoop opening;
    QTimer discovery;
    discovery.setInterval(50);
    QObject::connect(&discovery, &QTimer::timeout, &opening, [&] {
        const auto state = platform->environment();
        if (state.settingsActive && selectPermissionGuideWindow(state.windows, 0))
            opening.quit();
    });
    discovery.start();
    QTimer::singleShot(5000, &opening, &QEventLoop::quit);
    opening.exec();
    discovery.stop();
    require(platform->environment().settingsActive, "Settings must be active before testing focus");
    PermissionGuideWidget widget(identity);
    widget.resize(479, widget.heightForGuideWidth(479));
    const pid_t frontmost = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    platform->prepareWindow(&widget);
    widget.show();
    QApplication::processEvents();
    NSView* view = reinterpret_cast<NSView*>(widget.winId());
    require(view.window && !view.window.hidesOnDeactivate && view.window.hasShadow,
            "guide persists alongside Settings with a native shadow");
    require(!view.window.keyWindow &&
                NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == frontmost,
            "showing guide must not activate Snow Shot or take keyboard focus");
    std::unique_ptr<QMimeData> payload(widget.createDragMimeData());
    require(payload && payload->urls() == QList<QUrl>{identity.bundleUrl},
            "native bundle exports a file URL for Finder-compatible dragging");
    widget.hide();
    const auto grants = createAppPermissionBackend()->query();
    std::cout << "Native metadata tracking: screen-recording="
              << grants.granted(AppPermission::ScreenRecording)
              << ", accessibility=" << grants.granted(AppPermission::Accessibility) << std::endl;
    if (app.arguments().contains(QStringLiteral("--show-guidance"))) {
        host.hide();
        AppPermissionService permissions(std::make_unique<ProbePermissions>());
        PermissionGuideController controller(permissions);
        permissions.openSettings(AppPermission::ScreenRecording);
        QObject::connect(controller.widget(), &PermissionGuideWidget::dismissed, &app,
                         &QCoreApplication::quit);
        QTimer::singleShot(180000, &app, &QCoreApplication::quit);
        return app.exec();
    }
#ifdef SNOW_PERMISSION_GUIDE_BENCHMARK
    const auto state = platform->environment();
    require(state.settingsActive && selectPermissionGuideWindow(state.windows, 0).has_value(),
            "benchmark needs the visible Settings window");
    constexpr int samples = 200;
    QElapsedTimer elapsed;
    elapsed.start();
    for (int i = 0; i < samples; ++i) {
        const auto environment = platform->environment();
        const auto window = selectPermissionGuideWindow(environment.windows, 0);
        require(window.has_value(), "Settings must stay visible during measurement");
        static_cast<void>(
            permissionGuidePlacement(window->bounds, environment.availableScreens, 110));
    }
    std::cout << "Active native discovery and placement: "
              << static_cast<double>(elapsed.nsecsElapsed()) / samples / 1000000.0 << " ms/sample ("
              << samples << " samples)\n";
#endif
    return 0;
}
