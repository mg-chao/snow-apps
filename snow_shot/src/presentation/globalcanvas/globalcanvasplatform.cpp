#include "globalcanvasplatform.h"
#include <QGuiApplication>
#include <QWidget>
#ifdef Q_OS_WIN
#include "../../platform/windows/pinnedwindownative.h"
#elif defined(Q_OS_MACOS)
#import <AppKit/AppKit.h>
#endif

namespace snow_shot::presentation {
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
        native.collectionBehavior |= NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary;
        return native.ignoresMouseEvents == transparent;
    }
#else
    Q_UNUSED(window);
    Q_UNUSED(transparent);
#endif
    return true;
}
} // namespace snow_shot::presentation
