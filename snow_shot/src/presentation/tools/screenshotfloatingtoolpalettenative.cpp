#include "screenshotfloatingtoolpalettenative.h"

#include <QWidget>

#include <algorithm>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>

namespace {
HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}
} // namespace
#endif

Qt::WindowFlags screenshot_floating_palette_native::windowFlags() {
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                            Qt::WindowDoesNotAcceptFocus;
#if !defined(Q_OS_MACOS)
    flags |= Qt::NoDropShadowWindowHint;
#endif
    return flags;
}

bool screenshot_floating_palette_native::currentPhysicalCursorPosition(QPointF* position) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    POINT point{};
    if (GetCursorPos(&point) != 0) {
        if (position != nullptr) {
            *position = QPointF(point.x, point.y);
        }
        return true;
    }
#else
    Q_UNUSED(position);
#endif
    return false;
}

bool screenshot_floating_palette_native::currentWindowGeometry(WId windowId, QRect* geometry) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    RECT rect{};
    if (hwnd == nullptr || GetWindowRect(hwnd, &rect) == FALSE) {
        return false;
    }

    if (geometry != nullptr) {
        *geometry =
            QRect(rect.left, rect.top, std::max(1, static_cast<int>(rect.right - rect.left)),
                  std::max(1, static_cast<int>(rect.bottom - rect.top)));
    }
    return true;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(geometry);
    return false;
#endif
}

bool screenshot_floating_palette_native::moveWindowTo(WId windowId, const QPoint& topLeft) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    return hwnd != nullptr && SetWindowPos(hwnd, nullptr, topLeft.x(), topLeft.y(), 0, 0,
                                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(topLeft);
    return false;
#endif
}

bool screenshot_floating_palette_native::setKeyboardFocusEnabled(WId windowId, bool enabled) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }

    const LONG_PTR currentStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR nextStyle = enabled ? currentStyle & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE)
                                       : currentStyle | static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    if (currentStyle == nextStyle) {
        return true;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previousStyle = SetWindowLongPtr(hwnd, GWL_EXSTYLE, nextStyle);
    if (previousStyle == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }

    return SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                            SWP_NOACTIVATE) != FALSE;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(enabled);
    return false;
#endif
}

bool screenshot_floating_palette_native::activateWindow(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }

    SetActiveWindow(hwnd);
    return SetForegroundWindow(hwnd) != FALSE || GetForegroundWindow() == hwnd;
#else
    Q_UNUSED(windowId);
    return false;
#endif
}

void screenshot_floating_palette_native::setNativePaletteOwner(WId windowId, QWidget* owner) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return;
    }
    const HWND ownerHwnd = owner != nullptr ? toNativeHwnd(owner->winId()) : nullptr;
    SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
#else
    Q_UNUSED(windowId);
    Q_UNUSED(owner);
#endif
}
