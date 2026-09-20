#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTest>

#include <functional>
#include <iostream>
#include <stdexcept>

#include "antd_icons.h"
#include "widgets/context_menu.h"

#import <AppKit/AppKit.h>

namespace {
using adqt::widgets::AdContextMenu;
namespace icons = adqt::icons::antd::outlined;
namespace twotone_icons = adqt::icons::antd::twotone;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void duringTracking(const std::function<void()>& callback) {
    const auto work = callback;
    NSTimer* timer = [NSTimer timerWithTimeInterval:0.1
                                            repeats:NO
                                              block:^(NSTimer*) {
                                                work();
                                              }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSEventTrackingRunLoopMode];
}

bool requestsVisibleImage(NSMenuItem* item) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27.0, *)) {
        return item.preferredImageVisibility == NSMenuItemImageVisibilityVisible;
    }
#endif
    return true;
}

bool containsOpaqueNeutralColor(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() == 255 && color.red() == color.green() &&
                color.green() == color.blue()) {
                return true;
            }
        }
    }
    return false;
}

void nativeItemsAndIcons() {
    AdContextMenu menu;
    auto* edit = menu.addItem(QStringLiteral("Edit"), icons::Edit(), QKeySequence::Copy);
    auto* colored =
        menu.addItem(QStringLiteral("Colored"),
                     icons::Copy().withColors(adqt::icons::IconColors::primary(QColor(Qt::red))));
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::blue);
    menu.addAction(QIcon(pixmap), QStringLiteral("Bitmap"))->setIconVisibleInMenu(true);
    auto* submenu = menu.addSubMenu(QStringLiteral("Folder"), icons::Folder());
    submenu->addItem(QStringLiteral("Child"), icons::Copy());
    auto* multitone = menu.addItem(QStringLiteral("Multitone"), twotone_icons::Camera());
    menu.addSeparator();
    edit->setCheckable(true);
    edit->setChecked(true);
    colored->setEnabled(false);
    QSystemTrayIcon tray;
    tray.setContextMenu(&menu);
    NSMenu* native = menu.toNSMenu();
    [native update];
    require(native && native.delegate, "Qt must own the NSMenu delegate");
    require(native.numberOfItems == 6, "native menu contains every action and separator");
    NSMenuItem* first = [native itemAtIndex:0];
    require(requestsVisibleImage(first), "native tray icons explicitly request visibility");
    require(first.image && first.image.isTemplate, "monochrome icons become native templates");
    require(first.state == NSControlStateValueOn, "check state reaches NSMenu");
    require(first.keyEquivalent.length > 0, "native shortcut is present");
    require([native itemAtIndex:1].image && ![native itemAtIndex:1].image.isTemplate,
            "explicit colors remain colored");
    require(![native itemAtIndex:1].enabled, "disabled state reaches NSMenu");
    require([native itemAtIndex:2].image && ![native itemAtIndex:2].image.isTemplate,
            "ordinary QIcon images remain supported");
    // Cocoa asks Qt's delegate to attach submenus when preparing each displayed item.
    [native.delegate menu:native updateItem:[native itemAtIndex:3] atIndex:3 shouldCancel:NO];
    require(requestsVisibleImage([native itemAtIndex:1]) &&
                requestsVisibleImage([native itemAtIndex:2]) &&
                requestsVisibleImage([native itemAtIndex:3]),
            "colored, bitmap and submenu images explicitly request visibility");
    require([native itemAtIndex:3].submenu && [native itemAtIndex:3].image.isTemplate,
            "submenu icons remain templates");
    const QImage multitoneImage = multitone->icon().pixmap(QSize(32, 32), QIcon::Normal).toImage();
    require([native itemAtIndex:4].image && ![native itemAtIndex:4].image.isTemplate &&
                containsOpaqueNeutralColor(multitoneImage) && !multitoneImage.isNull(),
            "native multi-tone icons bind their primary layer to the Cocoa menu foreground");
    NSMenu* childNative = [native itemAtIndex:3].submenu;
    submenu->addItem(QStringLiteral("Added later"), icons::Edit());
    require(requestsVisibleImage([childNative itemAtIndex:1]),
            "new submenu items inherit native image visibility");
    edit->setIconVisibleInMenu(false);
    require(!first.image, "explicitly hidden Qt icons remain absent");
    edit->setIconVisibleInMenu(true);
    menu.setActionIcon(edit, {});
    require(!first.image, "clearing an icon updates NSMenu");
    menu.setActionIcon(edit, icons::Copy());
    require(first.image.isTemplate && requestsVisibleImage(first),
            "replacing an icon preserves native visibility");
    edit->setText(QStringLiteral("Renamed"));
    require([first.title isEqualToString:@"Renamed"], "renaming updates NSMenu");
    colored->setVisible(false);
    require([native itemAtIndex:1].hidden, "hidden state reaches NSMenu");

    // Removing items changes notification indices; replacements and new submenus
    // must still receive the image policy without revisiting unrelated siblings.
    menu.removeAction(edit);
    menu.setActionIcon(colored, icons::Edit());
    require([native itemAtIndex:0].image.isTemplate && requestsVisibleImage([native itemAtIndex:0]),
            "icon updates follow the native item after indices shift");
    QMenu replacement;
    replacement.addAction(adqt::icons::makeIcon(icons::Copy()), QStringLiteral("Replacement"))
        ->setIconVisibleInMenu(true);
    submenu->menuAction()->setMenu(&replacement);
    NSMenuItem* replaced = [native itemAtIndex:2];
    [native.delegate menu:native updateItem:replaced atIndex:2 shouldCancel:NO];
    require(replaced.submenu && requestsVisibleImage([replaced.submenu itemAtIndex:0]),
            "a replacement submenu receives the image policy");
    replacement.addAction(adqt::icons::makeIcon(icons::Edit()), QStringLiteral("Added"))
        ->setIconVisibleInMenu(true);
    require(requestsVisibleImage([replaced.submenu itemAtIndex:1]),
            "replacement submenu notifications keep subsequent icons visible");
    submenu->menuAction()->setMenu(submenu);
    tray.setContextMenu(nullptr);
}

void queuedCancellationAndLifetime() {
    AdContextMenu menu;
    menu.addItem(QStringLiteral("Cancelled"));
    QSignalSpy shown(&menu, &QMenu::aboutToShow);
    menu.popupAt(QPoint(100, 100));
    require(menu.isPopupVisible() && shown.isEmpty(), "popupAt queues presentation");
    menu.dismissPopup();
    QCoreApplication::processEvents();
    require(!menu.isPopupVisible() && shown.isEmpty(), "dismissal cancels queued presentation");
    auto* transient = new AdContextMenu;
    transient->addItem(QStringLiteral("Temporary"));
    transient->popupAt(QPoint(100, 100));
    delete transient;
    QCoreApplication::processEvents();
}

void nativeSelectionAndCancellation(QWidget* owner, bool initial) {
    AdContextMenu menu(owner);
    auto* action = menu.addItem(QStringLiteral("Select me"), icons::Edit());
    QSignalSpy shown(&menu, &QMenu::aboutToShow);
    QSignalSpy hidden(&menu, &QMenu::aboutToHide);
    QSignalSpy triggered(action, &QAction::triggered);
    bool tracking = false;
    bool enabled = false;
    NSMenu* native = menu.toNSMenu();
    duringTracking([&]() {
        tracking = menu.isPopupVisible() && !menu.isVisible();
        enabled = [native itemAtIndex:0].enabled;
        [native performActionForItemAtIndex:0];
        menu.dismissPopup();
    });
    const QPoint point = owner ? owner->mapToGlobal(QPoint(30, 30)) : QPoint(100, 100);
    QAction* selected = menu.execAt(point, initial ? action : nullptr);
    require(enabled, "native actions remain enabled, including in modal windows");
    require(tracking, "native popup tracks without showing a QWidget popup");
    require(selected == action && triggered.count() == 1, "execAt returns one activated QAction");
    require(shown.count() == 1 && hidden.count() == 1, "native lifecycle signals fire once");
    require(!menu.isPopupVisible(), "native popup is closed after selection");
    duringTracking([&]() { menu.dismissPopup(); });
    require(menu.execAt(point) == nullptr, "cancellation returns nullptr");
    require(shown.count() == 2 && hidden.count() == 2 && triggered.count() == 1,
            "reopening and cancellation preserve signal counts");
}

void selectionFromQueuedInvocation(QWidget& owner) {
    AdContextMenu menu(&owner);
    auto* action = menu.addItem(QStringLiteral("Queued selection"));
    QAction* result = nullptr;
    QSignalSpy triggered(action, &QAction::triggered);
    duringTracking([]() {
        for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
            NSEvent* key = [NSEvent keyEventWithType:type
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:NSProcessInfo.processInfo.systemUptime
                                        windowNumber:NSApp.keyWindow.windowNumber
                                             context:nil
                                          characters:@"\r"
                         charactersIgnoringModifiers:@"\r"
                                           isARepeat:NO
                                             keyCode:36];
            [NSApp postEvent:key atStart:NO];
        }
    });
    QMetaObject::invokeMethod(
        &menu, [&]() { result = menu.execAt(owner.mapToGlobal(QPoint(30, 30)), action); },
        Qt::QueuedConnection);
    QCoreApplication::sendPostedEvents(&menu, QEvent::MetaCall);
    require(result == action && triggered.count() == 1,
            "execAt delivers native keyboard selection before a queued invocation returns");
}

void dynamicSubmenuAndDestruction(QWidget& owner) {
    AdContextMenu menu(&owner);
    auto* submenu = menu.addSubMenu(QStringLiteral("Dynamic"), icons::Folder());
    QObject::connect(submenu, &QMenu::aboutToShow, submenu, [submenu]() {
        submenu->clear();
        submenu->addItem(QStringLiteral("Created on open"), icons::Edit());
    });
    NSMenu* native = submenu->toNSMenu();
    bool populated = false;
    duringTracking([&]() {
        populated = native.numberOfItems == 1 && [native itemAtIndex:0].image != nil &&
                    requestsVisibleImage([native itemAtIndex:0]);
        [native performActionForItemAtIndex:0];
        submenu->dismissPopup();
    });
    require(submenu->execAt(owner.mapToGlobal(QPoint(40, 40))) != nullptr && populated,
            "native aboutToShow populates dynamic submenu actions and icons");

    QPointer<AdContextMenu> trackingMenu = new AdContextMenu(&owner);
    trackingMenu->addItem(QStringLiteral("Destroyed while tracking"));
    duringTracking([&]() { delete trackingMenu.data(); });
    require(trackingMenu->execAt(owner.mapToGlobal(QPoint(40, 40))) == nullptr && !trackingMenu,
            "native tracking tolerates menu destruction before an action is selected");

    QPointer<AdContextMenu> doomed = new AdContextMenu(&owner);
    doomed->addItem(QStringLiteral("Close owner"));
    QObject::connect(doomed, &QMenu::triggered, &owner, [&]() { delete doomed.data(); });
    NSMenu* doomedNative = doomed->toNSMenu();
    duringTracking([&]() {
        [doomedNative performActionForItemAtIndex:0];
        if (doomed) {
            doomed->dismissPopup();
        }
    });
    require(doomed->execAt(owner.mapToGlobal(QPoint(40, 40))) == nullptr && !doomed,
            "action handlers can destroy a native menu safely");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    @autoreleasepool {
        try {
            require(QGuiApplication::platformName() == QStringLiteral("cocoa"), "requires Cocoa");
            nativeItemsAndIcons();
            queuedCancellationAndLifetime();
            QWidget owner;
            owner.resize(360, 240);
            [NSApp activate];
            owner.show();
            require(QTest::qWaitForWindowExposed(&owner), "owner window is exposed");
            nativeSelectionAndCancellation(&owner, false);
            nativeSelectionAndCancellation(&owner, true);
            nativeSelectionAndCancellation(nullptr, false);
            nativeSelectionAndCancellation(nullptr, true);
            owner.hide();
            owner.setWindowModality(Qt::ApplicationModal);
            owner.show();
            nativeSelectionAndCancellation(&owner, false);
            nativeSelectionAndCancellation(&owner, true);
            require(QApplication::activeModalWidget() == &owner, "test owner is application-modal");
            selectionFromQueuedInvocation(owner);
            for (NSAppearanceName name : {NSAppearanceNameAqua, NSAppearanceNameDarkAqua}) {
                NSApp.appearance = [NSAppearance appearanceNamed:name];
                nativeSelectionAndCancellation(&owner, false);
            }
            dynamicSubmenuAndDestruction(owner);
            std::cout << "Native context menu tests passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
