#include "snow_shot/platform/focusedfullscreenwindow.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <vector>

namespace snow_shot::platform {

bool focusedFullscreenWindowExists() {
    NSRunningApplication* frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (frontmost == nil || frontmost.processIdentifier <= 0) {
        return false;
    }
    const pid_t processId = frontmost.processIdentifier;

    CFArrayRef rawWindows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (rawWindows == nullptr) {
        return false;
    }
    NSArray<NSDictionary*>* windows = CFBridgingRelease(rawWindows);
    QVector<FocusedWindowSnapshot> snapshots;
    snapshots.reserve(windows.count);
    for (NSDictionary* window in windows) {
        CGRect bounds = CGRectNull;
        if (!CGRectMakeWithDictionaryRepresentation(
                (__bridge CFDictionaryRef)window[(id)kCGWindowBounds], &bounds) ||
            CGRectIsEmpty(bounds) || CGRectIsNull(bounds)) {
            continue;
        }
        snapshots.push_back(
            {[window[(id)kCGWindowOwnerPID] longLongValue],
             [window[(id)kCGWindowLayer] intValue],
             [window[(id)kCGWindowAlpha] doubleValue],
             QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height)});
    }

    uint32_t displayCount = 0;
    if (CGGetActiveDisplayList(0, nullptr, &displayCount) != kCGErrorSuccess || displayCount == 0) {
        return false;
    }
    std::vector<CGDirectDisplayID> displays(displayCount);
    if (CGGetActiveDisplayList(displayCount, displays.data(), &displayCount) != kCGErrorSuccess) {
        return false;
    }
    displays.resize(displayCount);
    QVector<QRectF> bounds;
    bounds.reserve(static_cast<qsizetype>(displayCount));
    for (CGDirectDisplayID display : displays) {
        const CGRect rect = CGDisplayBounds(display);
        bounds.push_back(QRectF(rect.origin.x, rect.origin.y, rect.size.width, rect.size.height));
    }
    return focusedWindowCoversDisplay(processId, snapshots, bounds);
}

} // namespace snow_shot::platform
