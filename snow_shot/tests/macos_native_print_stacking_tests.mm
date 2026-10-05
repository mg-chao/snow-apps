#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/screenshotprintservice.h"
#include "platform/macos/capturewindowlayers_p.h"

#import <AppKit/AppKit.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot::platform::detail;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void nativeSheetsInheritCaptureFloors() {
    QWindow screenshot;
    screenshot.setProperty(kScreenshotLayer, kOverlayLayer);
    QWindow toolbar;
    toolbar.setTransientParent(&screenshot);
    toolbar.setProperty(kScreenshotLayer, kToolbarLayer);
    QWindow popup;
    popup.setTransientParent(&toolbar);
    QWindow nested;
    nested.setTransientParent(&popup);
    ModalFloors floors{};
    for (auto family :
         {CaptureFamily::Screenshot, CaptureFamily::Pinned, CaptureFamily::GlobalCanvas}) {
        screenshot.setProperty(kCaptureFamily, static_cast<int>(family));
        toolbar.setProperty(kCaptureFamily, static_cast<int>(family));
        const auto owner = captureLayer(&screenshot);
        const auto highest = captureLayer(&nested);
        floors[owner.index()] = highest.layer + 1;
        const auto sheet = nativePanelLayer(owner, floors);
        require(sheet.family == family && captureWindowLevel(sheet) > captureWindowLevel(highest),
                "a native print sheet must cover every tool in its originating capture family");
        require(captureWindowLevel(nativePanelLayer(owner, floors, 2)) > captureWindowLevel(sheet),
                "a sheet nested in the print panel must cover the print panel");
        if (family != CaptureFamily::Screenshot) {
            require(captureWindowLevel(sheet) <
                        captureWindowLevel({CaptureFamily::Screenshot, kOverlayLayer}),
                    "printing from a pin or canvas must preserve capture-family ordering");
        }
    }
    require(!nativePanelLayer({}, floors).valid(),
            "ordinary native sheets must not acquire a capture window level");
}

NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.winId()).window;
}

void nativeSheetsFollowToolChanges() {
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    owner.resize(320, 200);
    snow_shot::platform::configureScreenshotOverlayWindow(&owner);
    QWidget toolbar(&owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    toolbar.resize(120, 40);
    snow_shot::platform::configureScreenshotToolbarWindow(&toolbar);
    owner.show();
    toolbar.show();
    QCoreApplication::processEvents();
    NSPanel* sheet = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 160, 100)
                                                styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    sheet.animationBehavior = NSWindowAnimationBehaviorUtilityWindow;
    [native(owner) beginSheet:sheet completionHandler:nil];
    QCoreApplication::processEvents();
    require(sheet.sheetParent == native(owner) && sheet.level > native(toolbar).level,
            "native sheets must enter their owner's capture family without a Qt show event");
    require(sheet.animationBehavior == NSWindowAnimationBehaviorUtilityWindow,
            "native sheet stacking must preserve AppKit's presentation animation");
    NSPanel* nested = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 120, 80)
                                                 styleMask:NSWindowStyleMaskTitled
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
    [sheet beginSheet:nested completionHandler:nil];
    QCoreApplication::processEvents();
    require(nested.sheetParent == sheet && nested.level > sheet.level,
            "a native sub-sheet must inherit capture ownership above its parent sheet");
    QWidget popup(&toolbar, Qt::Tool);
    popup.resize(80, 40);
    popup.show();
    QCoreApplication::processEvents();
    require(sheet.level > native(popup).level && nested.level > sheet.level,
            "new capture tools must update all native sheet levels in one synchronization");
    [sheet endSheet:nested];
    [nested orderOut:nil];
    [native(owner) endSheet:sheet];
    [sheet orderOut:nil];
    QCoreApplication::processEvents();
    require(sheet.level <= native(owner).level && nested.level <= native(owner).level,
            "ending native sheets must release their capture stacking overrides");
    [nested release];
    [sheet release];
}

void nativePrintPanelCoversToolbar() {
    for (bool pinned : {false, true}) {
        QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        owner.resize(320, 200);
        if (pinned)
            setPinnedWindowLayer(&owner, true);
        else
            snow_shot::platform::configureScreenshotOverlayWindow(&owner);
        QWidget toolbar(&owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        toolbar.resize(120, 40);
        // Pin toolbars inherit their pin's family through the transient owner.
        if (!pinned)
            snow_shot::platform::configureScreenshotToolbarWindow(&toolbar);
        QWidget popup(&toolbar, Qt::Tool);
        popup.resize(80, 40);
        QWidget unrelated(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
        unrelated.resize(40, 40);
        unrelated.show();
        owner.show();
        toolbar.show();
        popup.show();
        QCoreApplication::processEvents();
        NSWindow* ownerWindow = native(owner);
        NSWindow* toolbarWindow = native(toolbar);
        NSWindow* popupWindow = native(popup);
        const auto ownerLevel = ownerWindow.level;
        const auto toolbarLevel = toolbarWindow.level;
        const auto popupLevel = popupWindow.level;
        const auto unrelatedLevel = native(unrelated).level;
        const auto ownerFlags = owner.windowFlags();
        const auto toolbarFlags = toolbar.windowFlags();
        const auto requirePreserved = [&] {
            require(ownerWindow.level == ownerLevel && toolbarWindow.level == toolbarLevel &&
                        popupWindow.level == popupLevel && owner.windowFlags() == ownerFlags &&
                        toolbar.windowFlags() == toolbarFlags && native(owner) == ownerWindow &&
                        native(toolbar) == toolbarWindow &&
                        native(unrelated).level == unrelatedLevel,
                    "printing must preserve capture levels, flags, and native surfaces");
        };
        QImage image(80, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool inspected = false;
            bool completed = false;
            NSWindow* inspectedSheet = nil;
            QTimer inspect;
            QObject::connect(&inspect, &QTimer::timeout, &owner, [&] {
                NSWindow* sheet = ownerWindow.attachedSheet;
                if (!sheet || !sheet.visible || inspected)
                    return;
                inspected = true;
                inspectedSheet = [sheet retain];
                require(sheet.sheetParent == ownerWindow,
                        "the native print panel must belong to the originating capture");
                requirePreserved();
                require(sheet.level > toolbarWindow.level && sheet.level > popupWindow.level,
                        "the print panel must appear above the drawing toolbar and its popups");
                // A later Qt raise must not put the toolbar above the native sheet.
                toolbar.raise();
                QCoreApplication::processEvents();
                require(sheet.level > toolbarWindow.level && sheet.level > popupWindow.level,
                        "capture resynchronization must keep the print panel above its tools");
                [sheet cancelOperation:nil];
            });
            inspect.start(0);
            auto& printer = ScreenshotPrintService::shared();
            require(printer.printImage(
                        &owner, &owner, image,
                        [&](auto result) {
                            require(result.status == ScreenshotPrintService::Status::Cancelled,
                                    "cancelling the native print panel must cancel the request");
                            completed = true;
                        }),
                    "native printing must start and reopen after cancellation");
            QElapsedTimer timeout;
            timeout.start();
            while (!completed && timeout.elapsed() < 10000)
                QCoreApplication::processEvents();
            inspect.stop();
            require(inspected && completed && !printer.busy(),
                    "the native print panel must be inspected and cancelled without a printer job");
            QCoreApplication::processEvents();
            requirePreserved();
            require(inspectedSheet.level < ownerWindow.level,
                    "closing the print panel must release its elevated capture level");
            [inspectedSheet release];
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    nativeSheetsInheritCaptureFloors();
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        @autoreleasepool {
            nativeSheetsFollowToolChanges();
            nativePrintPanelCoversToolbar();
        }
    }
    return 0;
}
