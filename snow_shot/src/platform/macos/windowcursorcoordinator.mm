#include "windowcursorcoordinator.h"
#include "windowcursorcoordinator_p.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <memory>
#include <limits>

namespace snow_shot::platform::macos {
namespace {
NSView* nativeView(QWidget* widget) {
    // Never call winId(): prewarmed/pooled widgets may have no native surface.
    for (; widget; widget = widget->parentWidget()) {
        if (widget->internalWinId())
            return reinterpret_cast<NSView*>(widget->internalWinId());
    }
    return nil;
}

class CocoaCursorEvents final {
  public:
    ~CocoaCursorEvents() {
        for (id observer in observers)
            [NSNotificationCenter.defaultCenter removeObserver:observer];
        [observers release];
        if (cursorObserver) {
            CFRunLoopObserverInvalidate(cursorObserver);
            CFRelease(cursorObserver);
        }
        [observedCursor release];
    }

    void observe(detail::WindowCursorCoordinator* coordinator) {
        observers = [NSMutableArray new];
        for (NSNotificationName name in @[
                 NSMenuDidBeginTrackingNotification, NSMenuDidEndTrackingNotification,
                 NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
                 NSWindowDidMoveNotification, NSWindowDidResizeNotification,
                 NSWindowDidEndSheetNotification, NSApplicationDidBecomeActiveNotification,
                 NSApplicationDidResignActiveNotification
             ]) {
            id observer = [NSNotificationCenter.defaultCenter
                addObserverForName:name
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* note) {
                          if ([note.name isEqualToString:NSMenuDidBeginTrackingNotification])
                              ++trackingMenus;
                          else if ([note.name isEqualToString:NSMenuDidEndTrackingNotification] &&
                                   trackingMenus > 0)
                              --trackingMenus;
                          coordinator->invalidate();
                        }];
            [observers addObject:observer];
        }
        // AppKit's display-cycle tracking manager can reset a stationary cursor
        // directly, without an NSEvent or Qt event. Observe after its run-loop
        // work, and invalidate only on a native cursor change. No timer or idle
        // window hit-testing is needed. Menus/modals still pass through the same
        // coordinator ownership checks as every other invalidation.
        cursorObserver = CFRunLoopObserverCreateWithHandler(
            kCFAllocatorDefault, kCFRunLoopBeforeWaiting, true, std::numeric_limits<CFIndex>::max(),
            ^(CFRunLoopObserverRef, CFRunLoopActivity) {
              if (rememberCursor())
                  coordinator->invalidate();
            });
        CFRunLoopAddObserver(CFRunLoopGetMain(), cursorObserver, kCFRunLoopCommonModes);
    }

    bool rememberCursor() {
        NSCursor* cursor = NSCursor.currentCursor;
        if (cursor == observedCursor)
            return false;
        [cursor retain];
        [observedCursor release];
        observedCursor = cursor;
        return true;
    }

    bool suspended() const {
        return trackingMenus > 0 || NSApp.modalWindow != nil;
    }

  private:
    NSMutableArray* observers = nil;
    CFRunLoopObserverRef cursorObserver = nullptr;
    NSCursor* observedCursor = nil;
    int trackingMenus = 0;
};

QWidget* hoverTarget() {
    const NSInteger hit = [NSWindow windowNumberAtPoint:NSEvent.mouseLocation
                            belowWindowWithWindowNumber:0];
    if (!hit)
        return nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->internalWinId() && nativeView(widget).window.windowNumber == hit)
            return widget;
    }
    // A foreign window, native menu, or save panel is not ours to refresh.
    return nullptr;
}

void applyCursor(QWidget* target, bool grabbed) {
    NSView* view = nativeView(target);
    NSWindow* window = view.window;
    if (!NSApp.active || !window.visible || window.ignoresMouseEvents)
        return;
    const NSPoint position = window.mouseLocationOutsideOfEventStream;
    NSView* receiver =
        grabbed ? view
                : [window.contentView hitTest:[window.contentView.superview convertPoint:position
                                                                                fromView:nil]];
    // Qt has already selected/converted the cursor, including its override stack
    // and custom bitmap DPI. Its public NSResponder callback applies that cache
    // without QCocoaWindow::setWindowCursor's key/titled-utility-window gate.
    [receiver cursorUpdate:[NSEvent enterExitEventWithType:NSEventTypeCursorUpdate
                                                  location:position
                                             modifierFlags:NSEvent.modifierFlags
                                                 timestamp:NSProcessInfo.processInfo.systemUptime
                                              windowNumber:window.windowNumber
                                                   context:nil
                                               eventNumber:0
                                            trackingNumber:0
                                                  userData:nullptr]];
}
} // namespace

void configureWindowCursorUpdates(QWidget* window) {
    if (!window || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    static QPointer<detail::WindowCursorCoordinator> coordinator;
    if (!coordinator) {
        auto events = std::make_shared<CocoaCursorEvents>();
        coordinator = new detail::WindowCursorCoordinator(
            {[] { return bool(NSApp.active); }, [events] { return events->suspended(); },
             hoverTarget, [] { return QWidget::mouseGrabber(); },
             [](QWidget* widget) {
                 NSView* view = nativeView(widget);
                 return view && !view.hiddenOrHasHiddenAncestor && view.window.visible &&
                        !view.window.ignoresMouseEvents;
             },
             [events](QWidget* target, bool grabbed) {
                 applyCursor(target, grabbed);
                 events->rememberCursor();
             },
             [] { return NSEvent.pressedMouseButtons != 0; }},
            qApp);
        events->observe(coordinator);
    }
    coordinator->addWindow(window);
}
} // namespace snow_shot::platform::macos
