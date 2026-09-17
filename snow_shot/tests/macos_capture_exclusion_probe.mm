#include "macos_capture_exclusion_probe.h"
#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <memory>

std::function<bool(bool)> macosCaptureSharingProbe(QWidget* widget) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return [](bool) { return true; };
    NSWindow* window = reinterpret_cast<NSView*>(widget->winId()).window;
    window.sharingType = NSWindowSharingReadOnly;
    const auto policy = window.sharingType;
    auto retained =
        std::shared_ptr<NSWindow>([window retain], [](NSWindow* value) { [value release]; });
    return [retained, policy](bool excluded) {
        return retained.get().sharingType == (excluded ? NSWindowSharingNone : policy);
    };
}
