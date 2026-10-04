#include "window_surface_mac_p.h"

#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace adqt::widgets::detail {

void releaseMacWindowSurfaceCursor(QWindow* surface) {
    if (!surface || !surface->handle() ||
        QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    auto* view = reinterpret_cast<NSView*>(surface->winId());
    // QNSView retains its cursor but Qt 6.11.1 and 6.12.0 do not release that property
    // in dealloc. SurfaceAboutToBeDestroyed is the last point where it is live.
    // Use the property setter so its retain is balanced without touching Qt's
    // shared cursor cache or the application's override cursor stack.
    if ([view respondsToSelector:@selector(setCursor:)]) {
        [view setValue:nil forKey:@"cursor"];
    }
}

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
