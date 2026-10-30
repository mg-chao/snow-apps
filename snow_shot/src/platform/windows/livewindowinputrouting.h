// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PLATFORM_WINDOWS_LIVEWINDOWINPUTROUTING_H
#define SNOW_SHOT_PLATFORM_WINDOWS_LIVEWINDOWINPUTROUTING_H

#include <qt_windows.h>
#include <windowsx.h>
#include <optional>

namespace snow_shot::presentation::live_window_detail {
// The content host and transparent shell form one input surface. The shell owns
// interaction policy; only client mouse coordinates need host-to-shell mapping.
inline std::optional<LRESULT> forwardPointerMessage(HWND host, HWND shell, UINT message,
                                                    WPARAM wParam, LPARAM lParam) {
    if (!shell)
        return {};
    switch (message) {
    case WM_SETCURSOR:
        // DefWindowProc may ask the shell's owner (the host) to resolve its
        // cursor. Only forward requests targeting the host to avoid reentry.
        if (reinterpret_cast<HWND>(wParam) != host)
            return {};
        return SendMessageW(shell, message, reinterpret_cast<WPARAM>(shell), lParam);
    case WM_NCHITTEST:
    case WM_NCMOUSEMOVE:
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDOWN:
    case WM_NCMBUTTONUP:
    case WM_NCMBUTTONDBLCLK:
    case WM_NCXBUTTONDOWN:
    case WM_NCXBUTTONUP:
    case WM_NCXBUTTONDBLCLK:
        return SendMessageW(shell, message, wParam, lParam);
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(host, &point);
        ScreenToClient(shell, &point);
        return SendMessageW(shell, message, wParam, MAKELPARAM(point.x, point.y));
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_CONTEXTMENU:
        return SendMessageW(shell, message, wParam, lParam);
    default:
        return {};
    }
}
} // namespace snow_shot::presentation::live_window_detail
#endif
