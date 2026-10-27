// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/screenshotairdropservice.h"
#include "platform/macos/capturewindowlayers_p.h"

#import <AppKit/AppKit.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.winId()).window;
}
bool chooserStaysAboveSource(bool pinned) {
    using Service = ScreenshotAirDropService;
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    owner.resize(320, 200);
    if (pinned)
        snow_shot::platform::detail::setPinnedWindowLayer(&owner, true);
    else
        snow_shot::platform::configureScreenshotOverlayWindow(&owner);
    QWidget toolbar(&owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    toolbar.resize(160, 40);
    if (!pinned)
        snow_shot::platform::configureScreenshotToolbarWindow(&toolbar);
    owner.show();
    toolbar.show();
    QCoreApplication::processEvents();
    NSWindow* ownerWindow = native(owner);
    const auto ownerLevel = ownerWindow.level;
    const auto toolbarLevel = native(toolbar).level;
    const auto flags = owner.windowFlags();
    QImage image(60, 40, QImage::Format_ARGB32);
    image.fill(QColor(70, 130, 200, 128));
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool inspected = false, completed = false, unavailable = false;
        QTimer inspect;
        QObject::connect(&inspect, &QTimer::timeout, &owner, [&] {
            NSWindow* sheet = ownerWindow.attachedSheet;
            if (!sheet) {
                NSWindow* key = NSApp.keyWindow;
                for (NSWindow* parent = key.parentWindow; parent; parent = parent.parentWindow)
                    if (parent == ownerWindow) {
                        sheet = key;
                        break;
                    }
            }
            if (!sheet || !sheet.visible || inspected)
                return;
            inspected = true;
            require(sheet.level > native(toolbar).level,
                    "AirDrop chooser must cover its source's floating toolbar");
            toolbar.raise();
            QCoreApplication::processEvents();
            require(sheet.level > native(toolbar).level,
                    "raising the toolbar must preserve AirDrop chooser stacking");
            [sheet cancelOperation:nil];
        });
        inspect.start(10);
        auto& service = Service::shared();
        require(service.shareImage(
                    &owner, &owner, image, QStringLiteral("AirDrop-test.png"),
                    [&](Service::Result result) {
                        unavailable = result.status == Service::Status::Unavailable;

                        require(unavailable || result.status == Service::Status::Cancelled ||
                                    result.status == Service::Status::HandedOff,
                                "closing AirDrop must not report a confirmed successful share");
                        completed = true;
                    }),
                "AirDrop starts and can reopen after cancellation");
        QElapsedTimer timer;
        timer.start();
        while (!completed && timer.elapsed() < 15000)
            QCoreApplication::processEvents();
        inspect.stop();
        if (unavailable)
            return false;
        if (!inspected || !completed) {
            for (NSWindow* window in NSApp.windows)
                std::cerr << "native window class=" << object_getClassName(window)
                          << " title=" << QString::fromNSString(window.title).toStdString()
                          << " visible=" << window.visible << " level=" << window.level
                          << " sheet=" << window.sheetParent << " parent=" << window.parentWindow
                          << " owner=" << ownerWindow << '\n';
            std::cerr << "inspected=" << inspected << " completed=" << completed
                      << " busy=" << service.busy() << '\n';
        }
        require(inspected && completed && !service.busy(),
                "inspect and cancel native AirDrop without sending to a device");
        require(owner.windowFlags() == flags && native(owner) == ownerWindow &&
                    ownerWindow.level == ownerLevel && native(toolbar).level == toolbarLevel,
                "AirDrop preserves source windows and their native levels");
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return 77;
    @autoreleasepool {
        return chooserStaysAboveSource(false) && chooserStaysAboveSource(true) ? 0 : 77;
    }
}
