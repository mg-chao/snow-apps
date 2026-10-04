#include "physical_key_test_support.h"
#include "snow_shot/presentation/components/actionpopupmenu.h"
#include "widgets/button.h"

#include <QApplication>
#include <QEnterEvent>
#include <QPointer>
#include <QTest>

#include <functional>
#include <iostream>
#include <stdexcept>

#import <AppKit/AppKit.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

NSTimer* after(double seconds, std::function<void()> callback) {
    NSTimer* timer = [NSTimer timerWithTimeInterval:seconds
                                            repeats:NO
                                              block:^(NSTimer*) {
                                                callback();
                                              }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSEventTrackingRunLoopMode];
    return timer;
}

void postKey(NSString* characters, unsigned short keyCode) {
    for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        NSEvent* event = [NSEvent keyEventWithType:type
                                          location:NSZeroPoint
                                     modifierFlags:0
                                         timestamp:NSProcessInfo.processInfo.systemUptime
                                      windowNumber:NSApp.keyWindow.windowNumber
                                           context:nil
                                        characters:characters
                       charactersIgnoringModifiers:characters
                                         isARepeat:NO
                                           keyCode:keyCode];
        [NSApp postEvent:event atStart:NO];
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    @autoreleasepool {
        try {
            QWidget owner;
            owner.resize(360, 240);
            adqt::widgets::AdButton button(&owner);
            button.setGeometry(230, 180, 90, 32);
            QPointer<adqt::widgets::AdContextMenu> menu;
            QPointer<QAction> action;
            int creations = 0;
            int shown = 0;
            int hidden = 0;
            int triggered = 0;
            snow_shot::presentation::ActionPopupMenu controller(
                &button,
                [&]() {
                    ++creations;
                    menu = new adqt::widgets::AdContextMenu(&owner);
                    action = menu->addItem(QStringLiteral("Native button action"));
                    QObject::connect(menu, &QMenu::aboutToShow, &owner, [&] { ++shown; });
                    QObject::connect(menu, &QMenu::aboutToHide, &owner, [&] { ++hidden; });
                    QObject::connect(action, &QAction::triggered, &owner, [&] { ++triggered; });
                    return menu.data();
                },
                snow_shot::presentation::ActionPopupMenu::Placement::TopRight);
            [NSApp activate];
            owner.show();
            owner.activateWindow();
            require(QTest::qWaitForWindowExposed(&owner), "button owner is exposed");
            const QPoint local = button.rect().center();
            QEnterEvent enter(local, local, button.mapToGlobal(local));
            QApplication::sendEvent(&button, &enter);
            require(creations == 0 && !menu, "hover must not open native menus");

            button.click();
            require((menu && menu->isPopupVisible()) && shown == 0,
                    "button queues native presentation");
            bool stayedOpen = false;
            bool watchdogUsed = false;
            after(0.2, [&]() {
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(&button, &leave);
            });
            after(0.5, [&]() {
                stayedOpen = (menu && menu->isPopupVisible()) && !(menu && menu->isVisible());
                postKey(@"\x1b", 53);
            });
            NSTimer* watchdog = after(2, [&]() {
                watchdogUsed = true;
                if (menu)
                    menu->dismissPopup();
            });
            QCoreApplication::sendPostedEvents(menu.data(), QEvent::MetaCall);
            [watchdog invalidate];
            require(stayedOpen && !(menu && menu->isPopupVisible()) && !watchdogUsed,
                    "native Escape closes without hover timers");
            require(shown == 1 && hidden == 1 && triggered == 0,
                    "button cancellation emits one lifecycle pair");

            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(!menu && !action, "native cancellation releases the menu and its actions");

            PhysicalKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(&button, &key);
            require((menu && menu->isPopupVisible()), "keyboard opens a native button menu");
            after(0.2, []() { postKey(@"\r", 36); });
            watchdog = after(2, [&]() {
                watchdogUsed = true;
                if (menu)
                    menu->dismissPopup();
            });
            QCoreApplication::sendPostedEvents(menu.data(), QEvent::MetaCall);
            [watchdog invalidate];
            require(triggered == 1 && !(menu && menu->isPopupVisible()),
                    "native Return activates the keyboard-selected action once");

            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(!menu && !action && creations == 2,
                    "keyboard activation retires its fresh menu");

            button.click();
            button.hide();
            QCoreApplication::sendPostedEvents(menu.data(), QEvent::MetaCall);
            require(!(menu && menu->isPopupVisible()) && shown == 2,
                    "hiding the trigger cancels pending native presentation");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(!menu && !action && creations == 3,
                    "pending cancellation also releases resources");
            std::cout << "Native action popup tests passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
