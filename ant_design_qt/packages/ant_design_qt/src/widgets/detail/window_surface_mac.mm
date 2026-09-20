#include "window_surface_mac_p.h"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace adqt::widgets::detail {

void updateMacWindowSurfaceShadow(QWidget* surface) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    auto* view = reinterpret_cast<NSView*>(surface->winId());
    NSWindow* window = view.window;
    // Cocoa's shadow follows the painted alpha and never participates in hit testing.
    // Qt window flags own shadow visibility, including after native recreation.
    [window invalidateShadow];
}

} // namespace adqt::widgets::detail
