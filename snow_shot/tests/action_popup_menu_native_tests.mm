#include "snow_shot/presentation/components/actionpopupmenu.h"
#include "widgets/button.h"

#include <QApplication>
#include <QEnterEvent>
#include <QSignalSpy>
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
            adqt::widgets::AdContextMenu menu(&owner);
            QAction* action = menu.addItem(QStringLiteral("Native button action"));
            int creations = 0;
            snow_shot::presentation::ActionPopupMenu controller(
                &button,
                [&]() {
                    ++creations;
                    return &menu;
                },
                snow_shot::presentation::ActionPopupMenu::Placement::TopRight);
            [NSApp activate];
            owner.show();
            owner.activateWindow();
            require(QTest::qWaitForWindowExposed(&owner), "button owner is exposed");
            const QPoint local = button.rect().center();
            QEnterEvent enter(local, local, button.mapToGlobal(local));
            QApplication::sendEvent(&button, &enter);
            require(creations == 0 && !menu.isPopupVisible(), "hover must not open native menus");

            QSignalSpy shown(&menu, &QMenu::aboutToShow);
            QSignalSpy hidden(&menu, &QMenu::aboutToHide);
            QSignalSpy triggered(action, &QAction::triggered);
            button.click();
            require(menu.isPopupVisible() && shown.isEmpty(), "button queues native presentation");
            bool stayedOpen = false;
            bool watchdogUsed = false;
            after(0.2, [&]() {
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(&button, &leave);
            });
            after(0.5, [&]() {
                stayedOpen = menu.isPopupVisible() && !menu.isVisible();
                postKey(@"\x1b", 53);
            });
            NSTimer* watchdog = after(2, [&]() {
                watchdogUsed = true;
                menu.dismissPopup();
            });
            QCoreApplication::sendPostedEvents(&menu, QEvent::MetaCall);
            [watchdog invalidate];
            require(stayedOpen && !menu.isPopupVisible() && !watchdogUsed,
                    "native Escape closes without hover timers");
            require(shown.count() == 1 && hidden.count() == 1 && triggered.isEmpty(),
                    "button cancellation emits one lifecycle pair");

            QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(&button, &key);
            require(menu.isPopupVisible(), "keyboard opens a native button menu");
            after(0.2, []() { postKey(@"\r", 36); });
            watchdog = after(2, [&]() {
                watchdogUsed = true;
                menu.dismissPopup();
            });
            QCoreApplication::sendPostedEvents(&menu, QEvent::MetaCall);
            [watchdog invalidate];
            require(triggered.count() == 1 && !menu.isPopupVisible(),
                    "native Return activates the keyboard-selected action once");

            button.click();
            button.hide();
            QCoreApplication::sendPostedEvents(&menu, QEvent::MetaCall);
            require(!menu.isPopupVisible() && shown.count() == 2,
                    "hiding the trigger cancels pending native presentation");
            std::cout << "Native action popup tests passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
