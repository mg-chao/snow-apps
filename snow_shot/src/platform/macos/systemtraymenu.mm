#include "snow_shot/platform/macos/systemtraymenu.h"

#include <QGuiApplication>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>

#import <AppKit/AppKit.h>

namespace snow_shot::platform::macos {
namespace {
NSStatusBarButton* statusButton(NSView* view) {
    if ([view isKindOfClass:NSStatusBarButton.class]) {
        return static_cast<NSStatusBarButton*>(view);
    }
    for (NSView* child in view.subviews) {
        if (NSStatusBarButton* button = statusButton(child)) {
            return button;
        }
    }
    return nil;
}
} // namespace

void showSystemTrayMenu(QSystemTrayIcon* trayIcon, QMenu* menu) {
    // Qt emits Context again when the attached native menu starts tracking.
    if (!trayIcon || !menu || trayIcon->contextMenu() ||
        QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    @autoreleasepool {
        NSEvent* event = NSApp.currentEvent;
        if (event.type != NSEventTypeRightMouseDown) {
            return;
        }
        NSStatusBarButton* button = statusButton(event.window.contentView);
        if (!button) {
            return;
        }
        QPointer<QSystemTrayIcon> trayGuard(trayIcon);
        QPointer<QMenu> menuGuard(menu);
        // Qt signals on mouse-down, inside the button's tracking loop. Present in
        // default mode after that loop has consumed mouse-up; nesting menu tracking
        // here would swallow the release and leave the button tracking indefinitely.
        // The copied block retains the button; QPointers protect its Qt owners.
        [NSRunLoop.mainRunLoop performInModes:@[ NSDefaultRunLoopMode ]
                                        block:^{
                                          if (!trayGuard || !menuGuard || !trayGuard->isVisible() ||
                                              trayGuard->contextMenu() || !button.window) {
                                              return;
                                          }
                                          // Qt maps setContextMenu to NSStatusItem.menu. Its button
                                          // presents a pull-down; NSMenu.popUp is a different path.
                                          // https://developer.apple.com/documentation/appkit/nsstatusitem/menu
                                          trayGuard->setContextMenu(menuGuard);
                                          [button performClick:nil];
                                          // A permanent menu overrides left/middle click actions.
                                          if (trayGuard && trayGuard->contextMenu() == menuGuard) {
                                              trayGuard->setContextMenu(nullptr);
                                          }
                                        }];
    }
}
} // namespace snow_shot::platform::macos
