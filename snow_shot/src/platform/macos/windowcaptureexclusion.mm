#import <AppKit/AppKit.h>

#include "snow_shot/platform/macos/windowcaptureexclusion.h"

#include <QWidget>
#include <cstdint>

extern "C" void snow_macos_exclude_window(uint32_t window_id, uint8_t excluded);

namespace snow_shot::platform::macos {
bool setWindowExcludedFromCapture(QWidget* widget, bool excluded) {
    if (widget == nullptr) {
        return false;
    }
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    if (window == nil || window.windowNumber <= 0) {
        return false;
    }
    snow_macos_exclude_window(static_cast<uint32_t>(window.windowNumber), excluded ? 1 : 0);
    return true;
}
} // namespace snow_shot::platform::macos
