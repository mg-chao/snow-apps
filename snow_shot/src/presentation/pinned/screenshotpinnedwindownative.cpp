#include "screenshotpinnedwindownative.h"

#include <QCursor>
#include <QEventLoop>
#include <QGuiApplication>

#include <algorithm>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qpa/qplatformnativeinterface.h>
#include <qpa/qwindowsysteminterface.h>
#include <qt_windows.h>
// clang-format off
#include <commctrl.h>
// clang-format on

namespace {
constexpr UINT_PTR kSynchronizedResizeSubclassId = 0x5353525A; // "SSRZ"

HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

LRESULT CALLBACK synchronizedResizeSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR subclassId,
                                                DWORD_PTR referenceData) {
    Q_UNUSED(subclassId);
    bool synchronizeFrame = false;
    if (message == WM_WINDOWPOSCHANGED) {
        const auto* position = reinterpret_cast<const WINDOWPOS*>(lParam);
        const auto* interactiveResizeActive = reinterpret_cast<const bool*>(referenceData);
        synchronizeFrame = interactiveResizeActive != nullptr && *interactiveResizeActive &&
                           position != nullptr && (position->flags & SWP_NOSIZE) == 0 &&
                           (position->flags & SWP_NOCOPYBITS) != 0;
    }

    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (synchronizeFrame) {
        // Qt queues the QWidget geometry notification behind its native
        // procedure. Drain only non-input window-system events so the resized
        // canvas and camera are ready before publishing the layered surface.
        static_cast<void>(
            QWindowSystemInterface::flushWindowSystemEvents(QEventLoop::ExcludeUserInputEvents));
        static_cast<void>(
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE));
    }
    return result;
}
} // namespace
#endif

Qt::WindowFlags screenshot_pinned_window_native::windowFlags() {
    return Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint;
}

bool screenshot_pinned_window_native::applyClientGeometry(WId windowId, const QRect& geometry,
                                                          GeometryUpdate update) {
    if (!geometry.isValid() || geometry.isEmpty()) {
        return false;
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (update == GeometryUpdate::DiscardClientPixels) {
        flags |= SWP_NOCOPYBITS;
    }
    if (SetWindowPos(hwnd, nullptr, geometry.left(), geometry.top(), geometry.width(),
                     geometry.height(), flags) == FALSE) {
        return false;
    }
    return currentClientGeometry(windowId) == geometry;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(update);
    return true;
#endif
}

QRect screenshot_pinned_window_native::currentClientGeometry(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return {};
    }

    RECT clientRect{};
    POINT clientTopLeft{};
    if (GetClientRect(hwnd, &clientRect) == FALSE ||
        ClientToScreen(hwnd, &clientTopLeft) == FALSE) {
        return {};
    }
    return QRect(clientTopLeft.x, clientTopLeft.y,
                 std::max(1, static_cast<int>(clientRect.right - clientRect.left)),
                 std::max(1, static_cast<int>(clientRect.bottom - clientRect.top)));
#else
    Q_UNUSED(windowId);
    return {};
#endif
}

bool screenshot_pinned_window_native::applySystemResizeStyle(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }

    const LONG_PTR currentStyle = GetWindowLongPtr(hwnd, GWL_STYLE);
    if ((currentStyle & WS_THICKFRAME) == 0) {
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previousStyle =
            SetWindowLongPtr(hwnd, GWL_STYLE, currentStyle | WS_THICKFRAME);
        if (previousStyle == 0 && GetLastError() != ERROR_SUCCESS) {
            return false;
        }
    }

    return SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                            SWP_NOACTIVATE) != FALSE;
#else
    Q_UNUSED(windowId);
    return false;
#endif
}

bool screenshot_pinned_window_native::installSynchronizedResize(
    WId windowId, const bool* interactiveResizeActive) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    return hwnd != nullptr && interactiveResizeActive != nullptr &&
           SetWindowSubclass(hwnd, synchronizedResizeSubclassProc, kSynchronizedResizeSubclassId,
                             reinterpret_cast<DWORD_PTR>(interactiveResizeActive)) != FALSE;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(interactiveResizeActive);
    return true;
#endif
}

void screenshot_pinned_window_native::removeSynchronizedResize(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd != nullptr) {
        static_cast<void>(RemoveWindowSubclass(hwnd, synchronizedResizeSubclassProc,
                                               kSynchronizedResizeSubclassId));
    }
#else
    Q_UNUSED(windowId);
#endif
}

bool screenshot_pinned_window_native::applyCursor(Qt::CursorShape shape) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    QPlatformNativeInterface* nativeInterface = QGuiApplication::platformNativeInterface();
    if (nativeInterface == nullptr) {
        return false;
    }

    HCURSOR cursorHandle = static_cast<HCURSOR>(
        nativeInterface->nativeResourceForCursor(QByteArrayLiteral("hcursor"), QCursor(shape)));
    if (cursorHandle == nullptr) {
        return false;
    }

    SetCursor(cursorHandle);
    return GetCursor() == cursorHandle;
#else
    Q_UNUSED(shape);
    return false;
#endif
}

bool screenshot_pinned_window_native::synchronizeClientPaint(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    return hwnd != nullptr &&
           RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN) !=
               FALSE;
#else
    Q_UNUSED(windowId);
    return true;
#endif
}

bool screenshot_pinned_window_native::beginWindowMoveCapture(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }
    // HTCAPTION is handled by the application, so USER32 does not perform
    // its usual non-client activation before capture. A genuine pointer press
    // grants this process foreground rights; claim focus here so ordinary Qt
    // key events continue to reach the pinned window throughout the drag.
    static_cast<void>(SetForegroundWindow(hwnd));
    static_cast<void>(SetActiveWindow(hwnd));
    static_cast<void>(SetFocus(hwnd));
    SetCapture(hwnd);
    return GetCapture() == hwnd;
#else
    Q_UNUSED(windowId);
    return false;
#endif
}

void screenshot_pinned_window_native::endWindowMoveCapture(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd != nullptr && GetCapture() == hwnd) {
        static_cast<void>(ReleaseCapture());
    }
#else
    Q_UNUSED(windowId);
#endif
}
