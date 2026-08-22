#include "snow_shot/platform/windows/focusedfullscreenwindow.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <qt_windows.h>
#endif

#include <cstdlib>

namespace snow_shot::platform::windows {
namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
bool coordinatesMatch(LONG lhs, LONG rhs) {
    constexpr LONG tolerance = 1;
    return std::abs(lhs - rhs) <= tolerance;
}
#endif
} // namespace

bool focusedFullscreenWindowExists() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    HWND window = GetForegroundWindow();
    if (window == nullptr || IsWindow(window) == 0 || IsWindowVisible(window) == 0 ||
        IsIconic(window) != 0) {
        return false;
    }
    if (HWND root = GetAncestor(window, GA_ROOT); root != nullptr) {
        window = root;
    }
    if (window == GetDesktopWindow() || window == GetShellWindow()) {
        return false;
    }

    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
        cloaked != 0) {
        return false;
    }

    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                     sizeof(frame)))) {
        return false;
    }
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) {
        return false;
    }
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo) == 0) {
        return false;
    }

    const RECT& bounds = monitorInfo.rcMonitor;
    return coordinatesMatch(frame.left, bounds.left) &&
           coordinatesMatch(frame.top, bounds.top) &&
           coordinatesMatch(frame.right, bounds.right) &&
           coordinatesMatch(frame.bottom, bounds.bottom);
#else
    return false;
#endif
}

} // namespace snow_shot::platform::windows
