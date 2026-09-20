#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/context_menu.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSystemTrayIcon>
#include <QThread>

#import <AppKit/AppKit.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitFor(const std::function<bool()>& done, const char* message) {
    QElapsedTimer timeout;
    timeout.start();
    while (!done() && timeout.elapsed() < 3000) {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents();
        QThread::msleep(1);
    }
    require(done(), message);
}

NSStatusBarButton* statusButton(NSView* view) {
    if ([view isKindOfClass:NSStatusBarButton.class]) {
        return static_cast<NSStatusBarButton*>(view);
    }
    for (NSView* child in view.subviews) {
        if (auto* button = statusButton(child)) {
            return button;
        }
    }
    return nil;
}

void click(NSStatusBarButton* button, NSEventType down, NSEventType up, NSPoint point) {
    const NSPoint location = [button convertPoint:point toView:nil];
    for (NSEventType type : {down, up}) {
        NSEvent* event = [NSEvent mouseEventWithType:type
                                            location:location
                                       modifierFlags:0
                                           timestamp:NSProcessInfo.processInfo.systemUptime
                                        windowNumber:button.window.windowNumber
                                             context:nil
                                         eventNumber:0
                                          clickCount:1
                                            pressure:type == down ? 1 : 0];
        [NSApp postEvent:event atStart:NO];
    }
}

NSRect visibleMenuFrame() {
    for (NSWindow* window in NSApp.windows) {
        if (window.visible && window.level == NSPopUpMenuWindowLevel) {
            return window.frame;
        }
    }
    return NSZeroRect;
}

bool sameFrame(NSRect left, NSRect right) {
    return std::abs(left.origin.x - right.origin.x) < 1 &&
           std::abs(left.origin.y - right.origin.y) < 1 &&
           std::abs(left.size.width - right.size.width) < 1 &&
           std::abs(left.size.height - right.size.height) < 1;
}
} // namespace

int runNativeSystemTrayMenuTests(snow_shot::presentation::SystemTrayController& controller) {
    @autoreleasepool {
        try {
            using snow_shot::presentation::SystemTrayController;
            auto* tray = controller.findChild<QSystemTrayIcon*>();
            adqt::widgets::AdContextMenu* menu = nullptr;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->objectName() == QStringLiteral("systemTrayMenu")) {
                    menu = dynamic_cast<adqt::widgets::AdContextMenu*>(widget);
                }
            }
            require(tray && menu, "the native fixture must exercise the controller's actual tray");
            controller.setMenuOptions(snow_shot::storage::TraySettings().menuOptions());
            controller.show();
            NSStatusBarButton* button = nil;
            waitFor(
                [&]() {
                    for (NSWindow* window in NSApp.windows) {
                        if ((button = statusButton(window.contentView)) &&
                            NSIntersectsRect(window.frame, window.screen.frame)) {
                            return true;
                        }
                    }
                    return false;
                },
                "the native tray button must appear");
            const NSPoint center = NSMakePoint(NSMidX(button.bounds), NSMidY(button.bounds));

            QObject observer;
            int shown = 0;
            int hidden = 0;
            int mainRequests = 0;
            int settingsRequests = 0;
            bool attachedDuringShow = false;
            bool nativeVisible = false;
            bool selectAction = false;
            NSRect frame = NSZeroRect;
            QObject::connect(&controller, &SystemTrayController::showMainWindowRequested, &observer,
                             [&]() { ++mainRequests; });
            QObject::connect(&controller, &SystemTrayController::openFunctionSettingsRequested,
                             &observer, [&]() { ++settingsRequests; });
            QObject::connect(menu, &QMenu::aboutToHide, &observer, [&]() { ++hidden; });
            QObject::connect(menu, &QMenu::aboutToShow, &observer, [&]() {
                ++shown;
                attachedDuringShow = tray->contextMenu() == menu;
                // Inspect after AppKit has created the menu window, inside native tracking.
                NSTimer* timer = [NSTimer
                    timerWithTimeInterval:0.1
                                  repeats:NO
                                    block:^(NSTimer*) {
                                      frame = visibleMenuFrame();
                                      nativeVisible = menu->isPopupVisible() && !menu->isVisible();
                                      if (selectAction) {
                                          NSMenu* native = menu->toNSMenu();
                                          for (NSMenuItem* item in native.itemArray) {
                                              if ([item.title
                                                      isEqualToString:@"Show main interface"]) {
                                                  [native performActionForItemAtIndex:
                                                              [native indexOfItem:item]];
                                                  break;
                                              }
                                          }
                                      }
                                      [menu->toNSMenu() cancelTracking];
                                    }];
                [NSRunLoop.mainRunLoop addTimer:timer forMode:NSEventTrackingRunLoopMode];
            });

            const auto openAndClose = [&](NSPoint position) {
                const int previousShown = shown;
                const int previousHidden = hidden;
                frame = NSZeroRect;
                attachedDuringShow = nativeVisible = false;
                if (tray->contextMenu()) {
                    [button performClick:nil];
                } else {
                    click(button, NSEventTypeRightMouseDown, NSEventTypeRightMouseUp, position);
                }
                waitFor([&]() { return hidden > previousHidden; },
                        "native right-click must open and close the menu");
                require(shown == previousShown + 1 && hidden == previousHidden + 1,
                        "one right-click must produce exactly one native menu lifecycle");
                require(attachedDuringShow && nativeVisible && !NSIsEmptyRect(frame),
                        "the menu must track as the status item's attached native menu");
                require(!menu->isPopupVisible(), "native cancellation must close the menu");
                return frame;
            };

            // Reference placement comes from the documented Qt -> NSStatusItem.menu path.
            // Compare to AppKit itself instead of assuming a gap, inset, scale, or display origin.
            tray->setContextMenu(menu);
            const NSRect reference = openAndClose(center);
            tray->setContextMenu(nullptr);
            require(NSMaxY(reference) <= NSMinY(button.window.frame),
                    "the system's reference menu must be below the status bar");

            controller.setLeftClickAction(QStringLiteral("open_function_settings"));
            controller.setMiddleClickAction(QStringLiteral("show_main_window"));
            for (NSPoint position : {NSMakePoint(2, 2), NSMakePoint(NSMaxX(button.bounds) - 2,
                                                                    NSMaxY(button.bounds) - 2)}) {
                const NSRect actual = openAndClose(position);
                require(sameFrame(actual, reference),
                        "SnowShot must use AppKit's status-item placement at every click position");
                require(tray->contextMenu() == nullptr,
                        "the menu must detach after closing to restore configurable click actions");
                require(mainRequests == 0 && settingsRequests == 0,
                        "menu tracking must not dispatch a left or middle click action");
            }

            selectAction = true;
            openAndClose(center);
            waitFor([&]() { return mainRequests == 1; },
                    "native menu selection must dispatch once");
            require(tray->contextMenu() == nullptr,
                    "selecting an action must also detach the status menu");
            const int menuShows = shown;
            click(button, NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp, center);
            waitFor([&]() { return settingsRequests == 1; },
                    "native left click must dispatch once");
            require(shown == menuShows && mainRequests == 1,
                    "native left clicks must dispatch only their configured action");
            // Middle-click dispatch is exercised in the offscreen controller fixture.
            // AppKit does not route synthetic otherMouseDown to NSStatusBarButton.
            controller.hide();
            std::cout << "Native status-item placement and click tests passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
