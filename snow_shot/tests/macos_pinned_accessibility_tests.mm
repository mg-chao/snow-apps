// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/platform/macos/selectorlocalelements_p.h"
#include <QWidget>
#include <stdexcept>

void checkMacPinnedAccessibility(QWidget& window, const QRect& content) {
    using namespace snow_shot::platform::detail;
    auto* view = (__bridge NSView*)reinterpret_cast<void*>(window.winId());
    NSWindow* native = view.window;
    if (!native)
        throw std::runtime_error("pin has no native NSWindow");
    const auto displays = selectorDisplayGeometry();
    const qreal desktopTop = NSMaxY(NSScreen.screens.firstObject.frame);
    for (const auto& display : displays) {
        if (!display.capturedLogicalRect.contains(content.center()))
            continue;
        const QRectF frame = selectorLocalDesktopFrame(native.accessibilityFrame, desktopTop)
                                 .intersected(QRectF(display.capturedLogicalRect));
        const QRectF fallback = selectorLocalPixelFrame(frame, display);
        const QRectF image = selectorLocalPixelFrame(QRectF(content).intersected(frame), display);
        ScreenshotSelectorResult result;
        result.ok = true;
        result.mode = ScreenshotSelectorHitTestMode::WindowSubElement;
        result.nativeWindowId = static_cast<std::uintptr_t>(native.windowNumber);
        result.point = image.center().toPoint();
        result.displayId = display.nativeDisplayId;
        result.rects = {fallback};
        // Exercise the same local NSAccessibility hit and ancestry used after the
        // Rust worker's own-process fallback, without requesting AX permission.
        resolveSelectorLocalElements(result, displays);
        if (result.rects.size() < 2 || result.rects.front() != image ||
            result.rects.back() != fallback)
            throw std::runtime_error(
                "local macOS accessibility must select pin content before its window");
        return;
    }
    throw std::runtime_error("pin content must be on a captured display");
}
