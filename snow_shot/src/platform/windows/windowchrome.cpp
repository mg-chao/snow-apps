#include "snow_shot/platform/windows/windowchrome.h"

#if defined(Q_OS_WIN) || defined(_WIN32)

#include <QCursor>
#include <QPoint>
#include <QRect>
#include <QWidget>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 28301)
#endif
#include <dwmapi.h>
#include <qt_windows.h>
#include <windowsx.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace snow_shot::platform::windows {

namespace {
constexpr int SHADOW_BOTTOM_MARGIN = 1;
constexpr int RESIZE_BORDER_WIDTH = 5;
constexpr DWORD WINDOWS_10_2004_BUILD = 19041;

using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

bool supportsExcludeFromCapture() {
    static const bool supported = []() {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) {
            return false;
        }

        const auto rtlGetVersion =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion == nullptr) {
            return false;
        }

        RTL_OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtlGetVersion(&version) != 0) {
            return false;
        }

        return version.dwMajorVersion > 10 ||
               (version.dwMajorVersion == 10 && version.dwMinorVersion == 0 &&
                version.dwBuildNumber >= WINDOWS_10_2004_BUILD);
    }();
    return supported;
}

bool adjustClientRectForMaximizedWindow(const MSG* msg, qintptr* result) {
    // LPARAM carries a NCCALCSIZE_PARAMS pointer for WM_NCCALCSIZE.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);

    if (IsZoomed(msg->hwnd) != 0) {
        const HMONITOR monitor = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo) != 0) {
            params->rgrc[0] = monitorInfo.rcWork;
        }
    }

    *result = 0;
    return true;
}

bool handleNcCalcSize(const MSG* msg, qintptr* result) {
    if (msg->wParam != TRUE) {
        return false;
    }

    return adjustClientRectForMaximizedWindow(msg, result);
}

bool hitTestResizeBorder(const MSG* msg, qintptr* result) {
    RECT winRect{};
    GetWindowRect(msg->hwnd, &winRect);

    const int x = GET_X_LPARAM(msg->lParam);
    const int y = GET_Y_LPARAM(msg->lParam);

    const bool onLeft = (x >= winRect.left && x < winRect.left + RESIZE_BORDER_WIDTH);
    const bool onRight = (x <= winRect.right && x > winRect.right - RESIZE_BORDER_WIDTH);
    const bool onTop = (y >= winRect.top && y < winRect.top + RESIZE_BORDER_WIDTH);
    const bool onBottom = (y <= winRect.bottom && y > winRect.bottom - RESIZE_BORDER_WIDTH);

    if (onTop && onLeft) {
        *result = HTTOPLEFT;
        return true;
    }
    if (onTop && onRight) {
        *result = HTTOPRIGHT;
        return true;
    }
    if (onBottom && onLeft) {
        *result = HTBOTTOMLEFT;
        return true;
    }
    if (onBottom && onRight) {
        *result = HTBOTTOMRIGHT;
        return true;
    }
    if (onLeft) {
        *result = HTLEFT;
        return true;
    }
    if (onRight) {
        *result = HTRIGHT;
        return true;
    }
    if (onTop) {
        *result = HTTOP;
        return true;
    }
    if (onBottom) {
        *result = HTBOTTOM;
        return true;
    }

    return false;
}

bool isTitleBarDragArea(QWidget* titleBar) {
    if (titleBar == nullptr) {
        return false;
    }

    const QPoint globalPos = QCursor::pos();
    const QPoint localPos = titleBar->mapFromGlobal(globalPos);
    if (!titleBar->rect().contains(localPos)) {
        return false;
    }

    QWidget* const window = titleBar->window();
    if (window == nullptr) {
        return false;
    }

    const QWidget* const hitWidget = window->childAt(window->mapFromGlobal(globalPos));
    return hitWidget == titleBar || (hitWidget == nullptr && window == titleBar);
}

bool handleNcHitTest(QWidget* titleBar, const MSG* msg, qintptr* result) {
    // Let the DWM run its default hit-test first (for things like the
    // top-of-screen snap zone).
    LRESULT dpiResult = 0;
    if (DwmDefWindowProc(msg->hwnd, msg->message, msg->wParam, msg->lParam, &dpiResult) != 0) {
        *result = dpiResult;
        return true;
    }

    if (hitTestResizeBorder(msg, result)) {
        return true;
    }

    // Use QCursor::pos() which is already in Qt logical coordinates,
    // avoiding the DPI mismatch with physical-pixel WM_NCHITTEST coords.
    if (isTitleBarDragArea(titleBar)) {
        *result = HTCAPTION;
        return true;
    }

    *result = HTCLIENT;
    return true;
}
} // namespace

void setupDwmShadow(QWidget* window) {
    if (window == nullptr) {
        return;
    }

    // Ensure the native window handle exists.
    window->winId();
    const HWND hwnd = toNativeHwnd(window->winId());

    // Extend the frame into the client area so the DWM draws its shadow around
    // the window even though we remove the non-client area via WM_NCCALCSIZE.
    const MARGINS margins = {0, 0, 0, SHADOW_BOTTOM_MARGIN};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Tell the window manager to redraw the frame.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void bringWindowToForeground(QWidget* window) {
    if (window == nullptr) {
        return;
    }

    const HWND hwnd = toNativeHwnd(window->winId());
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

bool setWindowExcludedFromCapture(QWidget* window, bool excluded) {
    if (window == nullptr) {
        return false;
    }
    if (excluded && !supportsExcludeFromCapture()) {
        return false;
    }

    window->winId();
    const HWND hwnd = toNativeHwnd(window->winId());
    if (hwnd == nullptr) {
        return false;
    }

    const DWORD affinity = excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    return SetWindowDisplayAffinity(hwnd, affinity) != 0;
}

bool handleNativeWindowEvent(QWidget* titleBar, void* message, qintptr* result) {
    if (message == nullptr || result == nullptr) {
        return false;
    }

    const auto* msg = static_cast<const MSG*>(message);

    switch (msg->message) {

    case WM_NCCALCSIZE:
        return handleNcCalcSize(msg, result);

    case WM_NCHITTEST:
        return handleNcHitTest(titleBar, msg, result);

    default:
        return false;
    }
}

} // namespace snow_shot::platform::windows

#endif
