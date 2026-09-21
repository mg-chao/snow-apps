#ifndef SNOW_SHOT_PLATFORM_MACOS_SCREENSHOTWINDOWTARGET_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SCREENSHOTWINDOWTARGET_P_H

#include <CoreGraphics/CoreGraphics.h>
#include <QPoint>
#include <algorithm>
#include <span>

namespace snow_shot::platform::detail {
struct WindowTarget {
    CGWindowID id = 0;
    pid_t pid = 0;
    CGRect bounds{};
};
template <typename Accept>
inline WindowTarget selectWindowTarget(CFArrayRef windows, const QPoint* point, Accept accept) {
    if (!windows)
        return {};
    WindowTarget result;
    for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
        const auto window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, index));
        const auto number = [window](CFStringRef key, CFNumberType type, void* value) {
            const auto entry = static_cast<CFNumberRef>(CFDictionaryGetValue(window, key));
            return entry && CFNumberGetValue(entry, type, value);
        };
        pid_t pid = 0;
        int layer = 0;
        double alpha = 0;
        CGWindowID id = 0;
        if (!number(kCGWindowOwnerPID, kCFNumberIntType, &pid) ||
            !number(kCGWindowLayer, kCFNumberIntType, &layer) ||
            !number(kCGWindowAlpha, kCFNumberDoubleType, &alpha) ||
            !number(kCGWindowNumber, kCFNumberIntType, &id) || id == 0 || pid <= 0 || alpha <= 0)
            continue;
        CGRect bounds{};
        const auto rectangle =
            static_cast<CFDictionaryRef>(CFDictionaryGetValue(window, kCGWindowBounds));
        if (!rectangle || !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds) ||
            CGRectIsEmpty(bounds))
            continue;
        if (point && !CGRectContainsPoint(bounds, CGPointMake(point->x(), point->y())))
            continue;
        if (!accept(id, pid, layer))
            continue;
        result = {id, pid, bounds};
        break;
    }
    return result;
}

inline WindowTarget focusedWindowTarget(CFArrayRef windows, pid_t owner) {
    // Focused capture can target our own application, just like any other app.
    return owner > 0 ? selectWindowTarget(windows, nullptr,
                                          [owner](CGWindowID, pid_t pid, int layer) {
                                              return pid == owner && layer == 0;
                                          })
                     : WindowTarget{};
}

inline WindowTarget scrollWindowTarget(CFArrayRef windows, pid_t currentProcess,
                                       const QPoint& point) {
    // Scroll input must pass through our capture overlay to the application below.
    return selectWindowTarget(windows, &point, [currentProcess](CGWindowID, pid_t pid, int layer) {
        return pid != currentProcess && layer == 0;
    });
}

// AppKit hit testing accounts for window shape and ignoresMouseEvents, unlike
// bounding rectangles from WindowServer (which include decorative system surfaces).
// Step below an editing surface only when it is itself the frontmost hit.
template <typename HitTest>
inline CGWindowID recaptureWindowAtPoint(std::span<const CGWindowID> excludedWindows,
                                         HitTest hitTest) {
    CGWindowID below = 0;
    for (size_t index = 0; index <= excludedWindows.size(); ++index) {
        const CGWindowID hit = hitTest(below);
        if (!hit || hit == below)
            return 0;
        if (std::find(excludedWindows.begin(), excludedWindows.end(), hit) == excludedWindows.end())
            return hit;
        below = hit;
    }
    return 0;
}

// Resolve metadata for the exact native hit, without imposing scrolling's
// process/level policy or substituting a window underneath if it disappeared.
inline WindowTarget recaptureWindowTarget(CFArrayRef windows, CGWindowID hit) {
    return selectWindowTarget(windows, nullptr,
                              [hit](CGWindowID id, pid_t, int) { return id == hit; });
}
} // namespace snow_shot::platform::detail

#endif
