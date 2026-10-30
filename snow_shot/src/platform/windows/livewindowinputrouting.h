// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PLATFORM_WINDOWS_LIVEWINDOWINPUTROUTING_H
#define SNOW_SHOT_PLATFORM_WINDOWS_LIVEWINDOWINPUTROUTING_H

#include <qt_windows.h>
#include <windowsx.h>
#include <optional>

namespace snow_shot::presentation::live_window_detail {
// Call only while the pointer remains on the host/shell pair. Qt needs a client
// leave when entering a caption/resize region: non-client moves do not update
// QWidget hover. Client-to-client transitions still retain Qt's tracking.
inline bool suppressInternalPointerLeave(HWND shell, UINT message, LPARAM screenPosition) {
    return message == WM_NCMOUSELEAVE ||
           (message == WM_MOUSELEAVE &&
            SendMessageW(shell, WM_NCHITTEST, 0, screenPosition) == HTCLIENT);
}

// The content host and transparent shell form one input surface. The shell owns
// interaction policy; only client mouse coordinates need host-to-shell mapping.
inline std::optional<LRESULT> forwardPointerMessage(HWND host, HWND shell, UINT message,
                                                    WPARAM wParam, LPARAM lParam) {
    if (!shell)
        return {};
    switch (message) {
    case WM_MOUSEACTIVATE:
        // USER32 activates the recipient, even when pointer input is forwarded
        // to the shell. Activating the host deactivates the shell and cancels
        // its mouse grab. Deliver the press while leaving activation to the shell.
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        // DefWindowProc may ask the shell's owner (the host) to resolve its
        // cursor. Only forward requests targeting the host to avoid reentry.
        if (reinterpret_cast<HWND>(wParam) != host)
            return {};
        return SendMessageW(shell, message, reinterpret_cast<WPARAM>(shell), lParam);
    case WM_NCMOUSEMOVE: {
        const LRESULT result = SendMessageW(shell, message, wParam, lParam);
        // Non-client content cannot own Qt child hover. A suppressed internal
        // leave (for example while a tooltip opens) can consume USER32's
        // one-shot shell tracking while Qt still considers the client entered.
        // Report the non-client presence first, then complete the client exit.
        SendMessageW(shell, WM_MOUSELEAVE, 0, 0);
        return result;
    }
    case WM_NCHITTEST:
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
