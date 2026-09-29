#include "modal_mac_p.h"
#include "detail/window_modality.h"

#include <QWidget>
#include <QApplication>
#include <QPointer>
#include <QScopedValueRollback>
#include <QWindow>
#include <QSet>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

namespace adqt::widgets::detail {
namespace {
NSWindow* nativeWindow(QWidget* widget) {
    return widget && widget->internalWinId()
               ? reinterpret_cast<NSView*>(widget->internalWinId()).window
               : nil;
}

class CocoaModalSession;
QList<CocoaModalSession*> sessions;

void constrainOrdering(NSWindow* window, NSWindowOrderingMode& mode, NSInteger& relative);
bool usesDocumentPresentation(NSWindow* window);

void installNativeOrdering(NSWindow* window) {
    if (!window)
        return;
    // Install only on the concrete Qt class. Unmanaged windows forward unchanged;
    // the native object and its KVO identity remain intact across modal sessions.
    static QSet<Class> installed;
    Class windowClass = [window class];
    if (installed.contains(windowClass))
        return;
    const SEL selector = @selector(orderWindow:relativeTo:);
    const Method method = class_getInstanceMethod(windowClass, selector);
    const IMP original = method_getImplementation(method);
    const IMP replacement = imp_implementationWithBlock(^(
        NSWindow* receiver, NSWindowOrderingMode mode, NSInteger relative) {
      constrainOrdering(receiver, mode, relative);
      // Default inference depends on the native level. Resolve it at the
      // presentation boundary, including the first show and order-out, without
      // changing the caller's requested animation or overriding explicit choices.
      const bool resolveDefault = receiver.animationBehavior == NSWindowAnimationBehaviorDefault &&
                                  usesDocumentPresentation(receiver);
      if (resolveDefault)
          receiver.animationBehavior = NSWindowAnimationBehaviorDocumentWindow;
      reinterpret_cast<void (*)(id, SEL, NSWindowOrderingMode, NSInteger)>(original)(
          receiver, selector, mode, relative);
      if (resolveDefault && receiver.animationBehavior == NSWindowAnimationBehaviorDocumentWindow)
          receiver.animationBehavior = NSWindowAnimationBehaviorDefault;
    });
    class_replaceMethod(windowClass, selector, replacement, method_getTypeEncoding(method));
    installed.insert(windowClass);
}

class CocoaModalSession final : public MacModalSession {
  public:
    CocoaModalSession(QWidget* surface, QWidget* blocker) : surface_(surface), blocker_(blocker) {
        sessions.append(this);
        synchronize();
        if (!blocker_)
            return;
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
        if (eventMonitor_)
            [NSEvent removeMonitor:eventMonitor_];
        if (activationObserver_)
            [NSNotificationCenter.defaultCenter removeObserver:activationObserver_];
        sessions.removeOne(this);
    }

    void synchronize() override {
        installNativeOrdering(nativeSurface());
        for (QWidget* owner = blocker_ ? blocker_->parentWidget() : nullptr; owner;
             owner = owner->parentWidget()) {
            if (owner->isWindow())
                installNativeOrdering(nativeWindow(owner));
        }
    }

    NSWindow* nativeSurface() const {
        // Qt can replace the native window during show or a window-flag change.
        // The QWidget is the session's identity, not a retained obsolete NSWindow.
        return nativeWindow(surface_);
    }

    NSWindow* nativeOwner() const {
        return nativeWindow(blocker_ ? blocker_->parentWidget() : nullptr);
    }

    bool owns(NSWindow* window) const {
        for (QWidget* owner = blocker_ ? blocker_->parentWidget() : nullptr; owner;
             owner = owner->parentWidget()) {
            if (owner->isWindow() && nativeWindow(owner) == window)
                return true;
        }
        return false;
    }

    bool usesDocumentPresentation() const {
        return surface_ && surface_->windowModality() == Qt::NonModal &&
               (nativeSurface().styleMask & NSWindowStyleMaskTitled);
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
        if (activating_ || !nativeSurface())
            return;
        const QScopedValueRollback guard(activating_, true);
        [nativeSurface() makeKeyAndOrderFront:nil];
    }

    QPointer<QWidget> surface_;
    QPointer<QWidget> blocker_;
    id eventMonitor_ = nil;
    id activationObserver_ = nil;
    bool activating_ = false;
};

bool usesDocumentPresentation(NSWindow* window) {
    for (auto* session : sessions) {
        if (session->nativeSurface() == window)
            return session->usesDocumentPresentation();
    }
    return false;
}

void constrainOrdering(NSWindow* window, NSWindowOrderingMode& mode, NSInteger& relative) {
    if (mode == NSWindowOut || sessions.isEmpty())
        return;
    NSArray<NSNumber*>* windows = nil;
    const auto indexOf = [&](NSInteger number) {
        if (!windows)
            windows = [NSWindow windowNumbersWithOptions:0];
        return [windows indexOfObject:@(number)];
    };
    // Bound the requested position before AppKit applies it. A later update
    // notification is too late to prevent a frame of owner-over-modal occlusion.
    // Native levels remain the responsibility of Qt/the host's stacking policy.
    for (auto* session : sessions) {
        NSWindow* surface = session->nativeSurface();
        NSWindow* anchor = nil;
        NSWindowOrderingMode bound = NSWindowOut;
        if (surface.visible && session->owns(window)) {
            anchor = surface;
            bound = NSWindowBelow;
        } else if (surface == window && session->nativeOwner().visible) {
            anchor = session->nativeOwner();
            bound = NSWindowAbove;
        }
        if (!anchor)
            continue;
        const NSUInteger anchorIndex = indexOf(anchor.windowNumber);
        if (anchorIndex == NSNotFound)
            continue;
        const NSUInteger targetIndex =
            relative ? indexOf(relative) : (mode == NSWindowAbove ? 0 : NSNotFound);
        const bool crossesBound =
            bound == NSWindowBelow
                ? (mode == NSWindowAbove ? targetIndex <= anchorIndex : targetIndex < anchorIndex)
                : (mode == NSWindowBelow ? targetIndex >= anchorIndex : targetIndex > anchorIndex);
        if (crossesBound) {
            mode = bound;
            relative = anchor.windowNumber;
        }
    }
}
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
