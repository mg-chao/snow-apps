#ifndef SNOW_SHOT_PLATFORM_MACOS_SELECTORWINDOWTARGET_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SELECTORWINDOWTARGET_P_H

#include "screenshotwindowtarget_p.h"
#include "snow_shot/presentation/screenshottypes.h"

#include <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QPointF>
#include <cstdint>
#include <optional>
#include <vector>

namespace snow_shot::platform::detail {
inline QVector<CapturedDisplayModel> selectorDisplayGeometry() {
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0 || count > 128)
        return {};
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess)
        return {};
    QVector<CapturedDisplayModel> displays;
    for (uint32_t index = 0; index < count; ++index) {
        const auto id = ids[index];
        const CGRect bounds = CGDisplayBounds(id);
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
        if (!mode)
            continue;
        const size_t width = CGDisplayModeGetWidth(mode);
        const size_t height = CGDisplayModeGetHeight(mode);
        if (width && height) {
            CapturedDisplayModel display;
            display.nativeDisplayId = id;
            display.canvasUsesPoints = true;
            display.capturedLogicalRect =
                QRect(qRound(bounds.origin.x), qRound(bounds.origin.y), qRound(bounds.size.width),
                      qRound(bounds.size.height));
            display.physicalRect =
                QRect(display.capturedLogicalRect.topLeft(),
                      QSize(qRound(bounds.size.width *
                                   static_cast<double>(CGDisplayModeGetPixelWidth(mode)) /
                                   static_cast<double>(width)),
                            qRound(bounds.size.height *
                                   static_cast<double>(CGDisplayModeGetPixelHeight(mode)) /
                                   static_cast<double>(height))));
            displays.push_back(display);
        }
        CGDisplayModeRelease(mode);
    }
    return displays;
}

inline CGWindowID selectorWindowAtDesktopPoint(const QPointF& point,
                                               std::span<const std::uintptr_t> excluded) {
    // AppKit owns mouse hit testing on the UI thread. Quartz bounding rectangles
    // also contain click-through system surfaces and transparent window regions.
    const NSPoint nativePoint =
        NSMakePoint(point.x(), NSMaxY(NSScreen.screens.firstObject.frame) - point.y());
    std::vector<CGWindowID> ids;
    ids.reserve(excluded.size());
    for (const auto id : excluded)
        ids.push_back(static_cast<CGWindowID>(id));
    return recaptureWindowAtPoint(ids, [nativePoint](CGWindowID below) {
        return static_cast<CGWindowID>([NSWindow windowNumberAtPoint:nativePoint
                                         belowWindowWithWindowNumber:below]);
    });
}

inline std::optional<std::uintptr_t>
selectorWindowAtPhysicalPoint(const QPoint& point, quint32 displayId,
                              const QVector<CapturedDisplayModel>& displays,
                              std::span<const std::uintptr_t> excluded) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return std::nullopt;
    for (const auto& display : displays) {
        const QRect& pixels = display.physicalRect;
        const QRect& desktop = display.capturedLogicalRect;
        if ((displayId && display.nativeDisplayId != displayId) || !display.canvasUsesPoints ||
            pixels.isEmpty() || desktop.isEmpty() || !pixels.contains(point))
            continue;
        const QPointF position(
            desktop.x() +
                (point.x() - pixels.x()) * static_cast<qreal>(desktop.width()) / pixels.width(),
            desktop.y() +
                (point.y() - pixels.y()) * static_cast<qreal>(desktop.height()) / pixels.height());
        return selectorWindowAtDesktopPoint(position, excluded);
    }
    // A missing/stale display cannot identify a window in the frozen snapshot.
    return 0;
}
} // namespace snow_shot::platform::detail

#endif
