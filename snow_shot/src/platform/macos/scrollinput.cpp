#include "scrollinput_p.h"

#include <ApplicationServices/ApplicationServices.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <unistd.h>

namespace snow_shot::platform::macos {
namespace {
using Result = windows::ScrollInputResult;

bool containsPoint(const QRectF& bounds, const QPointF& point) {
    return point.x() >= bounds.left() && point.x() < bounds.right() && point.y() >= bounds.top() &&
           point.y() < bounds.bottom();
}

QVector<detail::ScrollDisplay> displays() {
    std::array<CGDirectDisplayID, 64> ids{};
    uint32_t count = 0;
    QVector<detail::ScrollDisplay> result;
    if (CGGetActiveDisplayList(static_cast<uint32_t>(ids.size()), ids.data(), &count) !=
        kCGErrorSuccess)
        return result;
    for (uint32_t i = 0; i < count; ++i) {
        const CGRect bounds = CGDisplayBounds(ids[i]);
        qreal scale = 1;
        if (const auto mode = CGDisplayCopyDisplayMode(ids[i])) {
            const auto width = CGDisplayModeGetWidth(mode);
            if (width > 0)
                scale = static_cast<qreal>(CGDisplayModeGetPixelWidth(mode)) /
                        static_cast<qreal>(width);
            CGDisplayModeRelease(mode);
        }
        result.push_back(
            {{bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height}, scale});
    }
    return result;
}

bool number(CFDictionaryRef dictionary, CFStringRef key, CFNumberType type, void* output) {
    const auto value = CFDictionaryGetValue(dictionary, key);
    return value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue(static_cast<CFNumberRef>(value), type, output);
}

QVector<detail::ScrollWindow> windows() {
    QVector<detail::ScrollWindow> result;
    const auto list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (list == nullptr)
        return result;
    for (CFIndex index = 0; index < CFArrayGetCount(list); ++index) {
        const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, index));
        const auto rawBounds = CFDictionaryGetValue(entry, kCGWindowBounds);
        CGRect bounds{};
        detail::ScrollWindow window;
        if (rawBounds == nullptr || CFGetTypeID(rawBounds) != CFDictionaryGetTypeID() ||
            !CGRectMakeWithDictionaryRepresentation(static_cast<CFDictionaryRef>(rawBounds),
                                                    &bounds) ||
            !number(entry, kCGWindowOwnerPID, kCFNumberIntType, &window.process) ||
            !number(entry, kCGWindowNumber, kCFNumberIntType, &window.id) ||
            !number(entry, kCGWindowLayer, kCFNumberIntType, &window.layer) ||
            !number(entry, kCGWindowAlpha, kCFNumberDoubleType, &window.opacity))
            continue;
        window.logicalBounds = {bounds.origin.x, bounds.origin.y, bounds.size.width,
                                bounds.size.height};
        result.push_back(window);
    }
    CFRelease(list);
    return result;
}

bool mayPost() {
    return AXIsProcessTrusted() && CGPreflightPostEventAccess();
}

Result post(const detail::ScrollRequest& request) {
    // Recheck just before dispatch: permission or the target can disappear after enumeration.
    if (!mayPost())
        return {Result::Status::PermissionDenied, 0};
    if (kill(request.process, 0) != 0)
        return {Result::Status::PostFailed, static_cast<quint32>(errno)};
    const auto event = detail::createScrollEvent(request);
    if (event == nullptr)
        return {Result::Status::PostFailed, 0};
    // Quartz has no delivery acknowledgement. Posted means handed to the target process,
    // not proof that its current view consumed the wheel event.
    CGEventPostToPid(request.process, event);
    CFRelease(event);
    return {Result::Status::Posted, 0};
}
} // namespace

namespace detail {
std::optional<QPointF> logicalScrollPosition(QPoint physicalPoint,
                                             const QVector<ScrollDisplay>& availableDisplays) {
    qreal desktopScale = 1;
    for (const auto& display : availableDisplays) {
        if (std::isfinite(display.backingScale))
            desktopScale = std::max(desktopScale, display.backingScale);
    }
    for (const auto& display : availableDisplays) {
        if (!std::isfinite(display.backingScale) || display.backingScale <= 0)
            continue;
        const auto origin = display.logicalBounds.topLeft();
        const QRectF physicalBounds(origin * desktopScale,
                                    display.logicalBounds.size() * display.backingScale);
        if (containsPoint(physicalBounds, physicalPoint))
            return origin +
                   (QPointF(physicalPoint) - physicalBounds.topLeft()) / display.backingScale;
    }
    return std::nullopt;
}

Result dispatchScrollingWheelStep(const QRect& selection, const QPoint& delta,
                                  const ScrollNativeApi& api) {
    if (selection.isEmpty() || delta.isNull())
        return {Result::Status::InvalidRequest, 0};
    if (!api.mayPost())
        return {Result::Status::PermissionDenied, 0};
    const auto position = logicalScrollPosition(selection.center(), api.displays());
    if (!position)
        return {Result::Status::CoordinateFailure, 0};
    for (const auto& window : api.windows()) {
        if (window.process <= 0 || window.process == api.ownProcess || window.id == 0 ||
            window.opacity <= 0 || window.layer < 0 ||
            !containsPoint(window.logicalBounds, *position))
            continue;
        // One Windows wheel notch is 120 units. Quartz positive axes scroll up/left,
        // whereas WM_MOUSEHWHEEL positive means right. Use three lines per notch.
        const auto lines = [](int value) {
            return value == 0
                       ? 0
                       : (value > 0 ? 1 : -1) *
                             std::max(1,
                                      static_cast<int>(std::abs(static_cast<qreal>(value)) / 40.0));
        };
        return api.post(
            {window.process, window.id, *position, QPoint(-lines(delta.x()), lines(delta.y()))});
    }
    return {Result::Status::TargetNotFound, 0};
}

CGEventRef createScrollEvent(const ScrollRequest& request) {
    const auto source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (source == nullptr)
        return nullptr;
    const auto event = CGEventCreateScrollWheelEvent2(source, kCGScrollEventUnitLine, 2,
                                                      request.lines.y(), request.lines.x(), 0);
    CFRelease(source);
    if (event != nullptr) {
        CGEventSetLocation(event, {request.position.x(), request.position.y()});
        CGEventSetFlags(event, 0);
        // Mouse-only window fields are ignored for wheel events. The target process and
        // location route the scroll to the application beneath the selection.
        CGEventSetIntegerValueField(event, kCGEventTargetUnixProcessID, request.process);
        // Preserve synthetic provenance; global gesture/effect taps must not treat this as input.
        CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, getpid());
    }
    return event;
}
} // namespace detail

Result sendScrollingWheelStep(const QRect& selection, const QPoint& delta) {
    return detail::dispatchScrollingWheelStep(selection, delta,
                                              {getpid(), mayPost, displays, windows, post});
}
} // namespace snow_shot::platform::macos
