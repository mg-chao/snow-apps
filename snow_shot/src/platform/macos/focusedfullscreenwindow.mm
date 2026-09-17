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
    QVector<FocusedWindowSnapshot> snapshots;
    const CFIndex windowCount = CFArrayGetCount(rawWindows);
    snapshots.reserve(static_cast<qsizetype>(windowCount));
    for (CFIndex index = 0; index < windowCount; ++index) {
        const auto window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(rawWindows, index));
        CGRect bounds = CGRectNull;
        const auto rectangle =
            static_cast<CFDictionaryRef>(CFDictionaryGetValue(window, kCGWindowBounds));
        if (rectangle == nullptr || !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds) ||
            CGRectIsEmpty(bounds) || CGRectIsNull(bounds)) {
            continue;
        }
        const auto number = [window](CFStringRef key, CFNumberType type, void* value) {
            const auto entry = static_cast<CFNumberRef>(CFDictionaryGetValue(window, key));
            return entry != nullptr && CFNumberGetValue(entry, type, value);
        };
        long long owner = 0;
        int layer = 0;
        double alpha = 0;
        if (!number(kCGWindowOwnerPID, kCFNumberLongLongType, &owner) ||
            !number(kCGWindowLayer, kCFNumberIntType, &layer) ||
            !number(kCGWindowAlpha, kCFNumberDoubleType, &alpha)) {
            continue;
        }
        snapshots.push_back(
            {owner, layer, alpha,
             QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height)});
    }
    CFRelease(rawWindows);

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
