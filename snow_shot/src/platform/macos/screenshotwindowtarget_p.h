#ifndef SNOW_SHOT_PLATFORM_MACOS_SCREENSHOTWINDOWTARGET_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SCREENSHOTWINDOWTARGET_P_H

#include <CoreGraphics/CoreGraphics.h>
#include <QPoint>

namespace snow_shot::platform::detail {
struct WindowTarget {
    CGWindowID id = 0;
    pid_t pid = 0;
    CGRect bounds{};
};
inline WindowTarget selectWindowTarget(CFArrayRef windows, pid_t owner, pid_t excludedOwner,
                                       const QPoint* point) {
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
            !number(kCGWindowNumber, kCFNumberIntType, &id) || pid <= 0 || pid == excludedOwner ||
            (owner && pid != owner) || layer != 0 || alpha <= 0)
            continue;
        CGRect bounds{};
        const auto rectangle =
            static_cast<CFDictionaryRef>(CFDictionaryGetValue(window, kCGWindowBounds));
        if (!rectangle || !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds) ||
            CGRectIsEmpty(bounds))
            continue;
        if (point && !CGRectContainsPoint(bounds, CGPointMake(point->x(), point->y())))
            continue;
        result = {id, pid, bounds};
        break;
    }
    return result;
}

inline WindowTarget focusedWindowTarget(CFArrayRef windows, pid_t owner) {
    // Focused capture can target our own application, just like any other app.
    return owner > 0 ? selectWindowTarget(windows, owner, 0, nullptr) : WindowTarget{};
}

inline WindowTarget scrollWindowTarget(CFArrayRef windows, pid_t currentProcess,
                                       const QPoint& point) {
    // Scroll input must pass through our capture overlay to the application below.
    return selectWindowTarget(windows, 0, currentProcess, &point);
}
} // namespace snow_shot::platform::detail

#endif
