#include "snow_shot/platform/windows/monitorgeometry.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#endif

namespace snow_shot::platform::windows {

QRect nativeMonitorRect(const QScreen& screen) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    // QScreen::handle() is the QPA screen object. Capture coordinates come from rcMonitor,
    // which is only available through the Windows native interface.
    const auto* native = screen.nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (native == nullptr || native->handle() == nullptr ||
        GetMonitorInfoW(native->handle(), &info) == FALSE) {
        return {};
    }
    const int width = static_cast<int>(info.rcMonitor.right - info.rcMonitor.left);
    const int height = static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top);
    if (width <= 0 || height <= 0) {
        return {};
    }
    return QRect(static_cast<int>(info.rcMonitor.left), static_cast<int>(info.rcMonitor.top), width,
                 height);
#else
    return {};
#endif
}

} // namespace snow_shot::platform::windows
