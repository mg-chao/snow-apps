#include "snow_shot/platform/macos/focusedfullscreenwindow.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace snow_shot::platform::macos {

bool focusedWindowFillsDisplay(qint64 foregroundProcessId, qint64 ownProcessId,
                               const QList<VisibleWindow>& windows, const QList<QRectF>& displays) {
    if (foregroundProcessId <= 0 || foregroundProcessId == ownProcessId) {
        return false;
    }
    for (const auto& window : windows) {
        if (window.processId != foregroundProcessId || window.layer != 0 ||
            window.bounds.isEmpty()) {
            continue;
        }
        return std::any_of(displays.cbegin(), displays.cend(), [&](const QRectF& display) {
            constexpr qreal tolerance = 1.0;
            return !display.isEmpty() &&
                   std::abs(window.bounds.left() - display.left()) <= tolerance &&
                   std::abs(window.bounds.top() - display.top()) <= tolerance &&
                   std::abs(window.bounds.right() - display.right()) <= tolerance &&
                   std::abs(window.bounds.bottom() - display.bottom()) <= tolerance;
        });
    }
    return false;
}

bool focusedFullscreenWindowExists() {
    @autoreleasepool {
        const pid_t foreground = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        const pid_t ownProcess = NSProcessInfo.processInfo.processIdentifier;
        if (foreground <= 0 || foreground == ownProcess) {
            return false;
        }
        uint32_t displayCount = 0;
        if (CGGetActiveDisplayList(0, nullptr, &displayCount) != kCGErrorSuccess ||
            displayCount == 0) {
            return false;
        }
        std::vector<CGDirectDisplayID> displayIds(displayCount);
        if (CGGetActiveDisplayList(displayCount, displayIds.data(), &displayCount) !=
            kCGErrorSuccess) {
            return false;
        }
        QList<QRectF> displays;
        for (uint32_t index = 0; index < displayCount; ++index) {
            const CGRect bounds = CGDisplayBounds(displayIds[index]);
            displays.push_back(
                {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height});
        }
        CFArrayRef rawWindows = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (rawWindows == nullptr) {
            return false;
        }
        QList<VisibleWindow> windows;
        for (NSDictionary* window in (__bridge NSArray*)rawWindows) {
            if ([window[(__bridge NSString*)kCGWindowAlpha] doubleValue] <= 0.0) {
                continue;
            }
            CGRect bounds{};
            if (!CGRectMakeWithDictionaryRepresentation(
                    (__bridge CFDictionaryRef)window[(__bridge NSString*)kCGWindowBounds],
                    &bounds)) {
                continue;
            }
            windows.push_back(
                {[window[(__bridge NSString*)kCGWindowOwnerPID] longLongValue],
                 [window[(__bridge NSString*)kCGWindowLayer] intValue],
                 { bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height }});
        }
        CFRelease(rawWindows);
        return focusedWindowFillsDisplay(foreground, ownProcess, windows, displays);
    }
}

} // namespace snow_shot::platform::macos
