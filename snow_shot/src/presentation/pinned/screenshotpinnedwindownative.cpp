#include "screenshotpinnedwindownative.h"

#include <QCursor>
#include <QEventLoop>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QWindow>

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

struct screenshot_pinned_window_native::SystemMoveKeyboard::Impl final : QObject {
    explicit Impl(QWidget* widget) : window(widget) {}

    QPointer<QWidget> window;
    QList<QKeyCombination> combinations;
#if defined(Q_OS_WIN) || defined(_WIN32)
    static thread_local Impl* active;
    HHOOK hook = nullptr;
    bool translating = false;
    bool handled = false;
    QSet<int> pressedKeys;

    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (!translating || window == nullptr) {
            return false;
        }
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
            return false;
        }
        auto* key = static_cast<QKeyEvent*>(event);
        const bool press = event->type() == QEvent::KeyPress;
        if ((press && combinations.contains(key->keyCombination())) ||
            (!press && pressedKeys.contains(key->key()))) {
            handled = true;
            if (press) {
                pressedKeys.insert(key->key());
            } else if (!key->isAutoRepeat()) {
                pressedKeys.remove(key->key());
            }
            QCoreApplication::sendEvent(window, event);
        }
        // Translation is only a probe for movement shortcuts. Unmatched keys
        // remain owned by USER32, including Escape/Enter to end the drag.
        return true;
    }

    static LRESULT CALLBACK filter(int code, WPARAM wParam, LPARAM lParam) {
        Impl* self = active;
        if (code == HC_ACTION && wParam == PM_REMOVE && self != nullptr && !self->translating &&
            self->window != nullptr) {
            auto* message = reinterpret_cast<MSG*>(lParam);
            GUITHREADINFO info{};
            info.cbSize = sizeof(info);
            if (message != nullptr && GetGUIThreadInfo(GetCurrentThreadId(), &info) != FALSE &&
                (info.flags & GUI_INMOVESIZE) != 0 &&
                info.hwndMoveSize == toNativeHwnd(self->window->winId()) &&
                (message->message == WM_KEYDOWN || message->message == WM_KEYUP ||
                 message->message == WM_SYSKEYDOWN || message->message == WM_SYSKEYUP)) {
                QScopedValueRollback<bool> guard(self->translating, true);
                self->handled = false;
                // USER32's modal move loop bypasses Qt's event dispatcher.
                // Use Qt's window procedure for layout/modifier translation,
                // then deliver the translated event before the loop resumes.
                SendMessageW(toNativeHwnd(self->window->winId()), message->message, message->wParam,
                             message->lParam);
                QWindowSystemInterface::flushWindowSystemEvents();
                if (self->handled) {
                    message->message = WM_NULL;
                    message->wParam = 0;
                    message->lParam = 0;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
#endif
};

#if defined(Q_OS_WIN) || defined(_WIN32)
thread_local screenshot_pinned_window_native::SystemMoveKeyboard::Impl*
    screenshot_pinned_window_native::SystemMoveKeyboard::Impl::active = nullptr;
#endif

screenshot_pinned_window_native::SystemMoveKeyboard::SystemMoveKeyboard(QWidget* window)
    : m_impl(std::make_unique<Impl>(window)) {}

screenshot_pinned_window_native::SystemMoveKeyboard::~SystemMoveKeyboard() {
    stop();
}

void screenshot_pinned_window_native::SystemMoveKeyboard::setKeyCombinations(
    const QList<QKeyCombination>& combinations) {
    m_impl->combinations = combinations;
}

bool screenshot_pinned_window_native::SystemMoveKeyboard::start() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (m_impl->hook != nullptr) {
        return true;
    }
    if (Impl::active != nullptr || m_impl->window == nullptr ||
        m_impl->window->windowHandle() == nullptr) {
        return false;
    }
    m_impl->hook = SetWindowsHookExW(WH_GETMESSAGE, Impl::filter, nullptr, GetCurrentThreadId());
    if (m_impl->hook == nullptr) {
        return false;
    }
    Impl::active = m_impl.get();
    m_impl->window->windowHandle()->installEventFilter(m_impl.get());
#endif
    return true;
}

void screenshot_pinned_window_native::SystemMoveKeyboard::stop() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (m_impl->hook != nullptr) {
        UnhookWindowsHookEx(m_impl->hook);
        m_impl->hook = nullptr;
        Impl::active = nullptr;
        if (m_impl->window != nullptr && m_impl->window->windowHandle() != nullptr) {
            m_impl->window->windowHandle()->removeEventFilter(m_impl.get());
        }
        m_impl->pressedKeys.clear();
    }
#endif
}

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

QRect screenshot_pinned_window_native::currentWindowGeometry(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return {};
    }

    RECT windowRect{};
    if (GetWindowRect(hwnd, &windowRect) != FALSE) {
        return QRect(QPoint(windowRect.left, windowRect.top),
                     QPoint(windowRect.right - 1, windowRect.bottom - 1));
    }
    return currentClientGeometry(windowId);
#else
    Q_UNUSED(windowId);
    return {};
#endif
}

std::optional<bool> screenshot_pinned_window_native::pointerInsideWindow(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    POINT pointer{};
    if (GetCursorPos(&pointer) == FALSE) {
        return std::nullopt;
    }
    const QRect nativeGeometry = currentWindowGeometry(windowId);
    if (!nativeGeometry.isValid() || nativeGeometry.isEmpty()) {
        return std::nullopt;
    }
    return nativeGeometry.contains(QPoint(pointer.x, pointer.y));
#else
    Q_UNUSED(windowId);
    return std::nullopt;
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

bool screenshot_pinned_window_native::activateWindow(WId windowId) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }

    const HWND foregroundHwnd = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread =
        foregroundHwnd != nullptr ? GetWindowThreadProcessId(foregroundHwnd, nullptr) : 0;
    const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                          AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;

    static_cast<void>(SetForegroundWindow(hwnd));
    static_cast<void>(BringWindowToTop(hwnd));
    static_cast<void>(SetActiveWindow(hwnd));
    static_cast<void>(SetFocus(hwnd));
    const bool activated = GetForegroundWindow() == hwnd;

    if (attached) {
        static_cast<void>(AttachThreadInput(currentThread, foregroundThread, FALSE));
    }
    return activated;
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

bool screenshot_pinned_window_native::synchronizeClientPaint(WId windowId,
                                                             PaintSynchronization synchronization) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(windowId);
    if (hwnd == nullptr) {
        return false;
    }
    UINT flags = RDW_UPDATENOW | RDW_ALLCHILDREN;
    if (synchronization == PaintSynchronization::InvalidateAndUpdate) {
        flags |= RDW_INVALIDATE;
    }
    return RedrawWindow(hwnd, nullptr, nullptr, flags) != FALSE;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(synchronization);
    return true;
#endif
}
