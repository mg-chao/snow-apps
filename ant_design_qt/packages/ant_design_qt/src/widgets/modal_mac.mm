#include "modal_mac_p.h"
#include "detail/window_modality.h"

#include <QWidget>
#include <QApplication>
#include <QPointer>
#include <QScopedValueRollback>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace adqt::widgets::detail {
namespace {
NSWindow* nativeWindow(QWidget* widget) {
    return widget && widget->internalWinId()
               ? reinterpret_cast<NSView*>(widget->internalWinId()).window
               : nil;
}

class CocoaModalSession final : public MacModalSession {
  public:
    CocoaModalSession(QWidget* surface, QWidget* blocker) : surface_(surface), blocker_(blocker) {
        synchronize();
        // Qt's Cocoa backend relies on an attached sheet to block its direct
        // owner's native activation. Our movable surface has no sheet, so stop
        // blocked input before AppKit orders or activates that owner.
        constexpr NSEventMask input =
            NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown |
            NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp |
            NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
            NSEventMaskOtherMouseDragged | NSEventMaskMouseMoved | NSEventMaskScrollWheel |
            NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskMagnify | NSEventMaskRotate |
            NSEventMaskSwipe;
        eventMonitor_ =
            [NSEvent addLocalMonitorForEventsMatchingMask:input
                                                  handler:^NSEvent*(NSEvent* event) {
                                                    if (!blocks(event.window))
                                                        return event;
                                                    if (event.type == NSEventTypeLeftMouseDown ||
                                                        event.type == NSEventTypeRightMouseDown ||
                                                        event.type == NSEventTypeOtherMouseDown ||
                                                        event.type == NSEventTypeKeyDown)
                                                        activateSurface();
                                                    return nil;
                                                  }];
        activationObserver_ = [NSNotificationCenter.defaultCenter
            addObserverForName:NSWindowDidBecomeKeyNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      if (blocks(static_cast<NSWindow*>(notification.object)))
                          activateSurface();
                    }];
    }

    ~CocoaModalSession() override {
        [NSEvent removeMonitor:eventMonitor_];
        [NSNotificationCenter.defaultCenter removeObserver:activationObserver_];
        detach();
    }

    void synchronize() override {
        NSWindow* surface = nativeWindow(surface_);
        NSWindow* owner = nativeWindow(blocker_ ? blocker_->parentWidget() : nullptr);
        if (surface == nativeSurface_ && owner == nativeOwner_)
            return;
        detach();
        if (!surface || !owner)
            return;
        nativeSurface_ = [surface retain];
        nativeOwner_ = [owner retain];
        previousParent_ = [surface.parentWindow retain];
        if (previousParent_ != owner) {
            [previousParent_ removeChildWindow:surface];
            // A native child remains above its owner even when the owner is
            // explicitly raised. Unlike a sheet, it can move independently.
            [owner addChildWindow:surface ordered:NSWindowAbove];
        }
    }

  private:
    bool blocks(NSWindow* native) const {
        if (!native || !surface_ || !surface_->isVisible() || !blocker_)
            return false;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (nativeWindow(widget) != native || !widget->windowHandle())
                continue;
            QWindow* blockingWindow = blockingModalWindow(widget->windowHandle());
            return blockingWindow && blockingWindow == blocker_->windowHandle();
        }
        return false;
    }

    void activateSurface() {
        if (activating_ || !nativeSurface_)
            return;
        const QScopedValueRollback guard(activating_, true);
        [nativeSurface_ makeKeyAndOrderFront:nil];
    }

    void detach() {
        if (nativeSurface_.parentWindow == nativeOwner_ && previousParent_ != nativeOwner_) {
            [nativeOwner_ removeChildWindow:nativeSurface_];
            [previousParent_ addChildWindow:nativeSurface_ ordered:NSWindowAbove];
        }
        [previousParent_ release];
        [nativeOwner_ release];
        [nativeSurface_ release];
        previousParent_ = nil;
        nativeOwner_ = nil;
        nativeSurface_ = nil;
    }

    QPointer<QWidget> surface_;
    QPointer<QWidget> blocker_;
    NSWindow* nativeSurface_ = nil;
    NSWindow* nativeOwner_ = nil;
    NSWindow* previousParent_ = nil;
    id eventMonitor_ = nil;
    id activationObserver_ = nil;
    bool activating_ = false;
};
} // namespace

std::unique_ptr<MacModalSession> createMacModalSession(QWidget* surface, QWidget* blocker) {
    return std::make_unique<CocoaModalSession>(surface, blocker);
}

void applyMacModalChrome(QWidget* widget) {
    auto* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    // Preserve the native title for window menus and accessibility, but avoid
    // drawing it over the modal's own header in the expanded content area.
    window.titleVisibility = NSWindowTitleHidden;
}

} // namespace adqt::widgets::detail
