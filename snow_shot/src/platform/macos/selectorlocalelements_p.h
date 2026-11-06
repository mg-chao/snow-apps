#ifndef SNOW_SHOT_PLATFORM_MACOS_SELECTORLOCALELEMENTS_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SELECTORLOCALELEMENTS_P_H

#include "selectorwindowtarget_p.h"
#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace snow_shot::platform::detail {
inline QRectF selectorLocalDesktopFrame(NSRect frame, qreal desktopTop) {
    return QRectF(frame.origin.x, desktopTop - NSMaxY(frame), frame.size.width, frame.size.height);
}

inline QRectF selectorLocalPixelFrame(const QRectF& frame, const CapturedDisplayModel& display) {
    const auto& desktop = display.capturedLogicalRect;
    const auto& pixels = display.physicalRect;
    const qreal scaleX = static_cast<qreal>(pixels.width()) / desktop.width();
    const qreal scaleY = static_cast<qreal>(pixels.height()) / desktop.height();
    const qreal left = std::floor(pixels.x() + (frame.left() - desktop.x()) * scaleX);
    const qreal top = std::floor(pixels.y() + (frame.top() - desktop.y()) * scaleY);
    const qreal right = std::ceil(pixels.x() + (frame.right() - desktop.x()) * scaleX);
    const qreal bottom = std::ceil(pixels.y() + (frame.bottom() - desktop.y()) * scaleY);
    return QRectF(left, top, right - left, bottom - top);
}

// Only local NSAccessibility objects enter this function. No AXUIElement messaging,
// permission checks, event pumping, or UI references escaping to selector workers.
inline QVector<QRectF> selectorLocalElementPath(id window, const QPoint& point,
                                                const CapturedDisplayModel& display,
                                                qreal desktopTop, const QRectF& fallback) {
    if (![NSThread isMainThread] || !window || display.capturedLogicalRect.isEmpty() ||
        display.physicalRect.isEmpty() || !display.canvasUsesPoints)
        return {};

    @autoreleasepool {
        @try {
            QElapsedTimer timer;
            timer.start();
            const auto valid = [](const QRectF& frame) {
                return std::isfinite(frame.x()) && std::isfinite(frame.y()) &&
                       std::isfinite(frame.width()) && std::isfinite(frame.height()) &&
                       !frame.isEmpty();
            };
            if (![window respondsToSelector:@selector(accessibilityFrame)] ||
                ![window respondsToSelector:@selector(accessibilityHitTest:)])
                return {};
            const QRectF bounds =
                selectorLocalDesktopFrame([window accessibilityFrame], desktopTop);
            if (!valid(bounds))
                return {};
            const QRectF visible = bounds.intersected(QRectF(display.capturedLogicalRect));
            // The worker already validated this window ID against its frozen snapshot.
            // Do not replace that fallback with children of a moved/resized window.
            if (visible.isEmpty() || selectorLocalPixelFrame(visible, display) != fallback)
                return {};
            const QPointF position = selectorDesktopPoint(point, display);
            if (!visible.contains(position))
                return {};
            id element =
                [window accessibilityHitTest:NSMakePoint(position.x(), desktopTop - position.y())];
            std::vector<id> seen;
            QVector<QRectF> path;
            // Native hit testing supplies the deepest element. Only walk its ancestry;
            // never enumerate an entire UI tree on the GUI thread. The time bound is
            // cooperative: individual local provider methods cannot be preempted.
            while (element && seen.size() < 64 && timer.nsecsElapsed() < 8'000'000) {
                if (std::find(seen.begin(), seen.end(), element) != seen.end())
                    return {};
                seen.push_back(element);
                if ([element respondsToSelector:@selector(isAccessibilityHidden)] &&
                    [element isAccessibilityHidden])
                    return {};
                if (element == window) {
                    path.push_back(fallback);
                    return path;
                }
                if ([element respondsToSelector:@selector(accessibilityFrame)]) {
                    const QRectF frame =
                        selectorLocalDesktopFrame([element accessibilityFrame], desktopTop);
                    if (valid(frame)) {
                        const QRectF clipped = frame.intersected(visible);
                        if (clipped.contains(position)) {
                            const QRectF pixels = selectorLocalPixelFrame(clipped, display);
                            if (pixels != fallback && !path.contains(pixels) && path.size() < 47)
                                path.push_back(pixels);
                        }
                    }
                }
                if (![element respondsToSelector:@selector(accessibilityParent)])
                    return {};
                element = [element accessibilityParent];
            }
        } @catch (NSException*) {
            // A broken local accessibility provider retains the validated window fallback.
        }
    }
    // Cycles, incomplete ancestry, and exhausted budgets must not publish unowned frames.
    return {};
}

inline void resolveSelectorLocalElements(ScreenshotSelectorResult& result,
                                         const QVector<CapturedDisplayModel>& displays) {
    if (![NSThread isMainThread] || QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        !result.ok || result.phase != ScreenshotSelectorResultPhase::Initial ||
        result.mode != ScreenshotSelectorHitTestMode::WindowSubElement ||
        result.stopReason != ScreenshotSelectorStopReason::Complete || result.rects.size() != 1 ||
        !result.nativeWindowId || *result.nativeWindowId == 0)
        return;
    // NSApplication only returns windows owned by this process. Looking up the exact
    // native hit also keeps excluded capture overlays out of the accessibility query.
    NSWindow* window =
        [NSApp windowWithWindowNumber:static_cast<NSInteger>(*result.nativeWindowId)];
    if (!window || !window.visible)
        return;
    const auto* display = selectorDisplayAtPhysicalPoint(result.point, result.displayId, displays);
    if (!display)
        return;
    QElapsedTimer timer;
    timer.start();
    auto path =
        selectorLocalElementPath(window, result.point, *display,
                                 NSMaxY(NSScreen.screens.firstObject.frame), result.rects.front());
    result.elapsedUs += static_cast<quint64>(timer.nsecsElapsed() / 1000);
    if (!path.isEmpty())
        result.rects = std::move(path);
}
} // namespace snow_shot::platform::detail

#endif
