#include "modal_mac_p.h"

#include <QWidget>

#import <AppKit/AppKit.h>

namespace adqt::widgets::detail {

void applyMacModalChrome(QWidget* widget) {
    auto* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    // Preserve the native title for window menus and accessibility, but avoid
    // drawing it over the modal's own header in the expanded content area.
    window.titleVisibility = NSWindowTitleHidden;
}

} // namespace adqt::widgets::detail
