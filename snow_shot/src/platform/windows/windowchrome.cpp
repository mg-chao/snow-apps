#include "snow_shot/platform/windows/windowchrome.h"
#include "snow_shot/platform/windowcaptureexclusion.h"

#if defined(Q_OS_WIN) || defined(_WIN32)

#include "windowcursorrefresh.h"

#include <QCursor>
#include <QAbstractButton>
#include <QVariant>
#include <QPointer>
#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QWidget>
#include <QTimer>
#include <QHash>

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
    const bool handledByDwm =
        DwmDefWindowProc(msg->hwnd, msg->message, msg->wParam, msg->lParam, &dpiResult) != 0;
    if (handledByDwm && !detail::isNativeCaptionControlHit(dpiResult)) {
        *result = dpiResult;
        return true;
    }

    if (IsZoomed(msg->hwnd) == 0 && hitTestResizeBorder(msg, result)) {
        return true;
    }

    // Only the visible child may opt into native caption behavior. This keeps
    // raised overlays in control and enables the system menu and Windows 11 Snap.
    if (titleBar != nullptr) {
        QWidget* hit =
            titleBar->window()->childAt(titleBar->window()->mapFromGlobal(QCursor::pos()));
        if (hit != nullptr && titleBar->isAncestorOf(hit)) {
            const int captionHit = hit->property("snowWindowCaptionHit").toInt();
            if (hit->isEnabled() && (captionHit == HTSYSMENU || captionHit == HTMAXBUTTON)) {
                *result = captionHit;
                return true;
            }
        }
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

bool detail::isNativeCaptionControlHit(qintptr hitTestResult) {
    return hitTestResult == HTSYSMENU || hitTestResult == HTMINBUTTON ||
           hitTestResult == HTMAXBUTTON || hitTestResult == HTCLOSE || hitTestResult == HTHELP;
}

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

bool supportsWindowCaptureExclusion() {
    return supportsExcludeFromCapture();
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

std::optional<bool> setWindowInputTransparent(QWidget* window, bool transparent) {
    if (window == nullptr || window->internalWinId() == 0 ||
        QGuiApplication::platformName() != QStringLiteral("windows")) {
        return std::nullopt;
    }
    const HWND hwnd = toNativeHwnd(window->internalWinId());
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((previous == 0 && GetLastError() != ERROR_SUCCESS) ||
        (transparent && (previous & WS_EX_LAYERED) == 0)) {
        return std::nullopt;
    }
    // Layered + transparent passes input to windows in other processes as well.
    // Do not change layering, activation, visibility, or any unrelated style bits.
    const LONG_PTR next =
        transparent ? previous | WS_EX_TRANSPARENT : previous & ~WS_EX_TRANSPARENT;
    if (next != previous) {
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(hwnd, GWL_EXSTYLE, next) == 0 && GetLastError() != ERROR_SUCCESS) {
            return std::nullopt;
        }
    }
    if (transparent) {
        if (QWidget* grabber = QWidget::mouseGrabber();
            grabber != nullptr && grabber->window() == window) {
            grabber->releaseMouse();
        }
        const HWND capture = GetCapture();
        if (capture == hwnd || (capture != nullptr && IsChild(hwnd, capture))) {
            ReleaseCapture();
        }
    }
    return (previous & WS_EX_TRANSPARENT) != 0;
}

bool refreshCursorUnderPointer() {
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return false;
    }
    POINT position{};
    return GetCursorPos(&position) && SetCursorPos(position.x, position.y);
}

bool detail::isUnderlyingCursorUpdate(quint32 event, qint32 object, quint32 sourceThread,
                                      quint32 callerThread, quint32 targetThread) {
    if (object != OBJID_CURSOR || targetThread == 0 ||
        (event != EVENT_OBJECT_NAMECHANGE && event != EVENT_OBJECT_SHOW &&
         event != EVENT_OBJECT_HIDE)) {
        return false;
    }
    // Cursor WinEvents describe the shared desktop cursor. Windows can publish the
    // transition from DWM/system threads, so sourceThread is not a window-owner identity.
    // Reject only our own outgoing changes while another app owns the pointer.
    return sourceThread != callerThread || targetThread == callerThread;
}

class CursorRefresh::Impl final : public QObject {
  public:
    explicit Impl(QObject* context) {
        connect(context, &QObject::destroyed, this, [this] {
            completed_ = {};
            cancelled_ = true;
            disarm();
        });
        if (QGuiApplication::platformName() != QStringLiteral("windows")) {
            return;
        }
        hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_NAMECHANGE, nullptr, cursorChanged,
                                0, 0, WINEVENT_OUTOFCONTEXT);
        if (hook_ != nullptr) {
            hooks().insert(hook_, this);
            SetCursor(nullptr);
        }
    }
    ~Impl() override {
        disarm();
    }

    void start(std::function<void(bool)> completed) {
        if (cancelled_) {
            return;
        }
        completed_ = std::move(completed);
        POINT position{};
        if (hook_ == nullptr || !GetCursorPos(&position) || !refreshCursorUnderPointer()) {
            finish(false);
            return;
        }
        // Route a real stationary mouse update. Do not send synthetic window messages:
        // their intermediate default cursor is not the target's queued widget selection.
        QTimer::singleShot(1000, this, [this] {
            if (completed_) {
                qWarning("Timed out waiting for a desktop cursor update before recapture");
                finish(false);
            }
        });
        if (notifiedTarget_ != nullptr && notifiedTarget_ == WindowFromPoint(position)) {
            queueCompletion();
        }
    }

  private:
    static QHash<HWINEVENTHOOK, Impl*>& hooks() {
        static thread_local QHash<HWINEVENTHOOK, Impl*> value;
        return value;
    }
    static void CALLBACK cursorChanged(HWINEVENTHOOK hook, DWORD event, HWND, LONG object, LONG,
                                       DWORD thread, DWORD) {
        if (object != OBJID_CURSOR || (event != EVENT_OBJECT_NAMECHANGE &&
                                       event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE)) {
            return;
        }
        auto* operation = hooks().value(hook, nullptr);
        if (operation == nullptr) {
            return;
        }
        POINT position{};
        if (!GetCursorPos(&position)) {
            return;
        }
        const HWND target = WindowFromPoint(position);
        const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
        if (!detail::isUnderlyingCursorUpdate(event, object, thread, GetCurrentThreadId(),
                                              targetThread)) {
            return;
        }
        operation->notifiedTarget_ = target;
        operation->queueCompletion();
    }
    void queueCompletion() {
        if (!completed_ || completionQueued_) {
            return;
        }
        completionQueued_ = true;
        // Leave WinEvent dispatch, then synchronize the pending desktop update once.
        // The target's SetCursor notification can precede its visible desktop cursor.
        // This is a single commit barrier, not cursor stability sampling. The coordinator
        // immediately snapshots the cursor and owns it through image capture.
        QTimer::singleShot(0, this, [this] {
            if (completed_) {
                finish(SUCCEEDED(DwmFlush()));
            }
        });
    }
    void disarm() {
        if (hook_ != nullptr) {
            hooks().remove(hook_);
            UnhookWinEvent(hook_);
            hook_ = nullptr;
        }
    }
    void finish(bool succeeded) {
        if (!completed_) {
            return;
        }
        disarm();
        auto completed = std::move(completed_);
        completed(succeeded);
    }
    std::function<void(bool)> completed_;
    HWINEVENTHOOK hook_ = nullptr;
    HWND notifiedTarget_ = nullptr;
    bool cancelled_ = false;
    bool completionQueued_ = false;
};

CursorRefresh::CursorRefresh(QObject* context) : impl_(std::make_unique<Impl>(context)) {}
CursorRefresh::~CursorRefresh() = default;
void CursorRefresh::refresh(std::function<void(bool)> completed) {
    impl_->start(std::move(completed));
}

bool isNativeWindowVisible(QWidget* window) {
    if (window == nullptr || window->internalWinId() == 0) {
        return false;
    }
    return IsWindowVisible(toNativeHwnd(window->internalWinId())) != FALSE;
}

bool flushWindowComposition() {
    return SUCCEEDED(DwmFlush());
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

    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE:
        if (titleBar != nullptr) {
            for (auto* button : titleBar->findChildren<QAbstractButton*>()) {
                if (button->property("snowWindowCaptionHit").toInt() != HTMAXBUTTON) {
                    continue;
                }
                const bool hovered = msg->message == WM_NCMOUSEMOVE && msg->wParam == HTMAXBUTTON;
                button->setProperty("snowNativeCaptionHover", hovered);
                button->update();
                if (hovered) {
                    TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT,
                                             msg->hwnd, 0};
                    TrackMouseEvent(&tracking);
                }
            }
        }
        // Keep DWM hover processing, including the Windows 11 Snap flyout.
        if (msg->message == WM_NCMOUSEMOVE || msg->message == WM_NCMOUSELEAVE) {
            LRESULT nativeResult = 0;
            if (DwmDefWindowProc(msg->hwnd, msg->message, msg->wParam, msg->lParam,
                                 &nativeResult)) {
                *result = nativeResult;
                return true;
            }
        }
        return false;

    case WM_NCLBUTTONDOWN:
        if (titleBar != nullptr && msg->wParam == HTMAXBUTTON) {
            QPointer<QAbstractButton> maximize;
            for (auto* button : titleBar->findChildren<QAbstractButton*>()) {
                if (button->property("snowWindowCaptionHit").toInt() == HTMAXBUTTON) {
                    maximize = button;
                    break;
                }
            }
            if (maximize != nullptr) {
                maximize->setDown(true);
                // USER32 owns tracking, cancellation and the maximize/restore command.
                *result = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, msg->lParam);
                if (maximize != nullptr) {
                    maximize->setDown(false);
                    maximize->setProperty("snowNativeCaptionHover", false);
                    maximize->update();
                }
                return true;
            }
        }
        return false;

    default:
        return false;
    }
}

} // namespace snow_shot::platform::windows

namespace snow_shot::platform {
bool setWindowExcludedFromCapture(QWidget* window, bool excluded) {
    return windows::setWindowExcludedFromCapture(window, excluded);
}

std::optional<std::uint32_t> captureWindowId(QWidget* window) {
    Q_UNUSED(window);
    // Windows uses display affinity; HWNDs are not macOS window IDs.
    return std::nullopt;
}
} // namespace snow_shot::platform

#endif
