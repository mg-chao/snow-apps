#include "globalcanvasplatform.h"
#include <QGuiApplication>
#include <QWidget>
#ifdef Q_OS_WIN
#include "../../platform/windows/pinnedwindownative.h"
#endif

namespace snow_shot::presentation {
#ifdef Q_OS_MACOS
NSWindowCollectionBehavior globalCanvasCollectionBehavior(NSWindowCollectionBehavior current) {
    // Qt tool windows can move to the active Space. AppKit rejects combining that
    // policy with joining all Spaces, and fullscreen roles are mutually exclusive.
    return (current & ~(NSWindowCollectionBehaviorMoveToActiveSpace |
                        NSWindowCollectionBehaviorFullScreenPrimary |
                        NSWindowCollectionBehaviorFullScreenNone)) |
           NSWindowCollectionBehaviorCanJoinAllSpaces |
           NSWindowCollectionBehaviorFullScreenAuxiliary;
}
#endif

bool setGlobalCanvasInputTransparent(QWidget* window, bool transparent) {
#ifdef Q_OS_WIN
    return screenshot_pinned_window_native::setInputTransparent(window->winId(), transparent);
#elif defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        NSView* view = reinterpret_cast<NSView*>(window->winId());
        NSWindow* native = view.window;
        if (native == nil)
            return false;
        native.ignoresMouseEvents = transparent;
        native.hidesOnDeactivate = NO;
        native.collectionBehavior = globalCanvasCollectionBehavior(native.collectionBehavior);
        return native.ignoresMouseEvents == transparent;
    }
#else
    Q_UNUSED(window);
    Q_UNUSED(transparent);
#endif
    return true;
}
} // namespace snow_shot::presentation
