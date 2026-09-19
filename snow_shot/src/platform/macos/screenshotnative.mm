#include "snow_shot/platform/screenshotnative.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>

namespace snow_shot::platform {
namespace {
struct WindowTarget {
    CGWindowID id = 0;
    pid_t pid = 0;
    CGRect bounds{};
};
WindowTarget windowTarget(pid_t owner, const QPoint* point) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
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
            !number(kCGWindowNumber, kCFNumberIntType, &id) || pid <= 0 ||
            pid == NSProcessInfo.processInfo.processIdentifier || (owner && pid != owner) ||
            layer != 0 || alpha <= 0)
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
    CFRelease(windows);
    return result;
}
} // namespace
quint32 screenshotDisplayAtCursor() {
    CGEventRef event = CGEventCreate(nullptr);
    if (!event)
        return 0;
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    return CGGetDisplaysWithPoint(point, 1, &display, &count) == kCGErrorSuccess && count ? display
                                                                                          : 0;
}
quint32 screenshotFocusedWindow() {
    const pid_t pid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    return pid > 0 ? windowTarget(pid, nullptr).id : 0;
}
bool screenshotScrollPermission() {
    return AXIsProcessTrusted();
}
ScrollInputResult sendScreenshotScroll(const QRect& selection, const QPoint& delta) {
    if (selection.isEmpty() || delta.isNull())
        return {ScrollInputResult::Status::InvalidRequest, 0};
    if (!screenshotScrollPermission())
        return {ScrollInputResult::Status::PostFailed, 1};
    const QPoint center = selection.center();
    const WindowTarget target = windowTarget(0, &center);
    if (!target.id)
        return {ScrollInputResult::Status::TargetNotFound, 0};

    CGEventRef event =
        CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, delta.y(), -delta.x());
    if (!event)
        return {ScrollInputResult::Status::PostFailed, 0};
    NSEvent* associated = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:target.id
                                              context:nil
                                          eventNumber:0
                                           clickCount:0
                                             pressure:0];
    CGEventRef routed = associated.CGEvent ? CGEventCreateCopy(associated.CGEvent) : nullptr;
    if (!routed) {
        CFRelease(event);
        return {ScrollInputResult::Status::PostFailed, 0};
    }
    CGEventSetType(routed, kCGEventScrollWheel);
    for (const auto field : {kCGScrollWheelEventDeltaAxis1, kCGScrollWheelEventDeltaAxis2,
                             kCGScrollWheelEventPointDeltaAxis1, kCGScrollWheelEventPointDeltaAxis2,
                             kCGScrollWheelEventIsContinuous})
        CGEventSetIntegerValueField(routed, field, CGEventGetIntegerValueField(event, field));
    for (const auto field :
         {kCGScrollWheelEventFixedPtDeltaAxis1, kCGScrollWheelEventFixedPtDeltaAxis2})
        CGEventSetDoubleValueField(routed, field, CGEventGetDoubleValueField(event, field));
    // Posting directly to a PID bypasses WindowServer's coordinate conversion.
    // CoreGraphics exports this bridge but omits it from the public SDK. Resolve
    // it at runtime: if unavailable, fail without warping the user's pointer.
    using SetWindowLocation = void (*)(CGEventRef, CGPoint);
    static const auto setWindowLocation =
        reinterpret_cast<SetWindowLocation>(dlsym(RTLD_DEFAULT, "CGEventSetWindowLocation"));
    if (!setWindowLocation) {
        CFRelease(routed);
        CFRelease(event);
        return {ScrollInputResult::Status::PostFailed, 0};
    }
    CGEventSetLocation(routed, CGPointMake(center.x(), center.y()));
    setWindowLocation(routed, CGPointMake(center.x() - target.bounds.origin.x,
                                          center.y() - target.bounds.origin.y));
    CGEventPostToPid(target.pid, routed);
    CFRelease(routed);
    CFRelease(event);
    return {ScrollInputResult::Status::Posted, 0};
}
} // namespace snow_shot::platform
