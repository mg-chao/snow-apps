#include "../../presentation/pinned/livewindownative_p.h"

#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <mutex>
#include <qt_windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace snow_shot::presentation {
namespace {
QString nativeText(const char* text) {
    return QCoreApplication::translate("LiveWindowSource", text);
}
HWND hwnd(quintptr value) {
    return reinterpret_cast<HWND>(value); // NOLINT(performance-no-int-to-ptr)
}
QRect desktopBounds(HWND window) {
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
        !GetWindowRect(window, &rect))
        return {};
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}
bool validWindow(HWND window) {
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        window == GetDesktopWindow() || window == GetShellWindow() ||
        GetAncestor(window, GA_ROOT) != window)
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked)
        return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT))
        return false;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    return wcscmp(name, L"Progman") != 0 && wcscmp(name, L"WorkerW") != 0;
}

class WindowsLiveWindow final : public live_window_detail::NativeWindow {
  public:
    explicit WindowsLiveWindow(LiveWindowTarget target) : m_target(std::move(target)) {}
    ~WindowsLiveWindow() override {
        stopPresentation();
        stopMonitoring();
    }
    bool initialize(QString* error) override {
        if (inspect().state != LiveWindowState::Live) {
            if (error)
                *error = nativeText(QT_TRANSLATE_NOOP(
                    "LiveWindowSource", "The selected window is no longer available."));
            return false;
        }
        m_destroyHook =
            SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, sourceDestroyed,
                            static_cast<DWORD>(m_target.processId), 0, WINEVENT_OUTOFCONTEXT);
        if (m_destroyHook)
            destroyHooks().insert(m_destroyHook, this);
        m_locationHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, sourceDestroyed,
            static_cast<DWORD>(m_target.processId), 0, WINEVENT_OUTOFCONTEXT);
        if (m_locationHook)
            destroyHooks().insert(m_locationHook, this);
        return true;
    }
    live_window_detail::NativeState inspect() override {
        if (m_closed) {
            stopMonitoring();
            return {LiveWindowState::Closed, {}, {}};
        }
        const HWND source = hwnd(m_target.nativeId);
        DWORD process = 0;
        if (!IsWindow(source) || !GetWindowThreadProcessId(source, &process) ||
            process != static_cast<DWORD>(m_target.processId)) {
            m_closed = true;
            stopMonitoring();
            return {LiveWindowState::Closed, {}, {}};
        }
        if (IsIconic(source))
            return {LiveWindowState::Minimized, {}, {}};
        const QRect bounds = desktopBounds(source);
        if (bounds.isEmpty())
            return {LiveWindowState::Unavailable,
                    {},
                    nativeText(QT_TRANSLATE_NOOP(
                        "LiveWindowSource",
                        "The window content is temporarily unavailable. Retrying…"))};
        return {LiveWindowState::Live, bounds, {}};
    }
    bool present(const LiveWindowPresentation& presentation, const QRectF& crop,
                 const QRectF& initialBounds, QString* error) override {
        Q_UNUSED(initialBounds);
        if (!presentation.owner || presentation.nativeContentGeometry.isEmpty()) {
            if (error)
                *error = nativeText(QT_TRANSLATE_NOOP(
                    "LiveWindowSource", "The live window display could not be initialized."));
            return false;
        }
        const HWND shell = reinterpret_cast<HWND>(presentation.owner->winId());
        if (m_shell && m_shell != shell)
            stopPresentation();
        m_owner = presentation.owner;
        if (!m_host) {
            static std::once_flag once;
            static ATOM windowClass = 0;
            std::call_once(once, [] {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.style = CS_DBLCLKS;
                wc.lpfnWndProc = windowProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"SnowShotLiveWindowHost";
                wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                windowClass = RegisterClassExW(&wc);
                BufferedPaintInit();
            });
            if (!windowClass)
                return fail(error);
            m_originalOwner = reinterpret_cast<HWND>(GetWindowLongPtrW(shell, GWLP_HWNDPARENT));
            m_host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                     MAKEINTRESOURCEW(windowClass), L"", WS_POPUP, 0, 0, 1, 1,
                                     m_originalOwner, nullptr, GetModuleHandleW(nullptr), this);
            if (!m_host)
                return fail(error);
            const MARGINS glass{-1, -1, -1, -1};
            if (FAILED(DwmExtendFrameIntoClientArea(m_host, &glass)))
                return fail(error);
            m_shell = shell;
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR previous =
                SetWindowLongPtrW(shell, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(m_host));
            if (!previous && GetLastError() != ERROR_SUCCESS)
                return fail(error);
        }
        if (!m_thumbnail &&
            FAILED(DwmRegisterThumbnail(m_host, hwnd(m_target.nativeId), &m_thumbnail)))
            return fail(error);
        m_background = presentation.backgroundColor;
        const auto native = inspect();
        if (native.state != LiveWindowState::Live) {
            stopPresentation();
            return true;
        }
        RECT full{};
        SIZE thumbnailSize{};
        if (!GetWindowRect(hwnd(m_target.nativeId), &full) ||
            FAILED(DwmQueryThumbnailSourceSize(m_thumbnail, &thumbnailSize)))
            return fail(error);
        const QRect& geometry = presentation.nativeContentGeometry;
        const auto mapping =
            live_window_detail::cropMapping(crop, QRectF(QPointF(), native.desktopBounds.size()),
                                            QRectF(QPointF(), geometry.size()));
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        if (!mapping.source.isEmpty())
            properties.dwFlags |= DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE;
        properties.opacity = 255;
        properties.fVisible = presentation.visible && !mapping.source.isEmpty();
        properties.fSourceClientAreaOnly = FALSE;
        // Depending on the source's nonclient rendering, DWM reports either
        // the visible frame or the complete native window including resizing
        // margins. Align either representation with WGC's visible-frame origin.
        const bool visibleFrame = thumbnailSize.cx == native.desktopBounds.width() &&
                                  thumbnailSize.cy == native.desktopBounds.height();
        const qreal scaleX = visibleFrame ? 1
                             : full.right > full.left
                                 ? qreal(thumbnailSize.cx) / (full.right - full.left)
                                 : 1;
        const qreal scaleY = visibleFrame ? 1
                             : full.bottom > full.top
                                 ? qreal(thumbnailSize.cy) / (full.bottom - full.top)
                                 : 1;
        const QPointF offset = visibleFrame ? QPointF()
                                            : QPointF(native.desktopBounds.x() - full.left,
                                                      native.desktopBounds.y() - full.top);
        const QRectF source(
            QPointF((mapping.source.x() + offset.x()) * scaleX,
                    (mapping.source.y() + offset.y()) * scaleY),
            QSizeF(mapping.source.width() * scaleX, mapping.source.height() * scaleY));
        properties.rcSource = {qRound(source.left()), qRound(source.top()), qRound(source.right()),
                               qRound(source.bottom())};
        properties.rcDestination = {
            qRound(mapping.destination.left()), qRound(mapping.destination.top()),
            qRound(mapping.destination.right()), qRound(mapping.destination.bottom())};
        if (FAILED(DwmUpdateThumbnailProperties(m_thumbnail, &properties)))
            return fail(error);
        const LONG_PTR style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                               (presentation.clickThrough ? WS_EX_TRANSPARENT : 0);
        SetWindowLongPtrW(m_host, GWL_EXSTYLE, style);
        SetLayeredWindowAttributes(
            m_host, 0,
            static_cast<BYTE>(
                qRound(qBound(qreal(0), presentation.effectiveOpacity, qreal(1)) * 255)),
            LWA_ALPHA);
        SetWindowPos(m_host, presentation.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, geometry.x(),
                     geometry.y(), geometry.width(), geometry.height(),
                     SWP_NOACTIVATE | (presentation.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // Ownership keeps the transparent Qt shell and all its controls above
        // the DWM destination while the pair follows the same topmost policy.
        InvalidateRect(m_host, nullptr, FALSE);
        m_presenting = presentation.visible;
        return true;
    }
    bool presenting() const override {
        return m_presenting;
    }
    void stopPresentation() override {
        m_presenting = false;
        if (m_thumbnail) {
            DwmUnregisterThumbnail(m_thumbnail);
            m_thumbnail = nullptr;
        }
        if (m_shell && IsWindow(m_shell) &&
            reinterpret_cast<HWND>(GetWindowLongPtrW(m_shell, GWLP_HWNDPARENT)) == m_host)
            SetWindowLongPtrW(m_shell, GWLP_HWNDPARENT,
                              reinterpret_cast<LONG_PTR>(m_originalOwner));
        m_shell = nullptr;
        if (m_host) {
            DestroyWindow(m_host);
            m_host = nullptr;
        }
        m_owner = nullptr;
    }

  private:
    void stopMonitoring() {
        for (auto* hook : {&m_destroyHook, &m_locationHook}) {
            if (*hook) {
                destroyHooks().remove(*hook);
                UnhookWinEvent(*hook);
                *hook = nullptr;
            }
        }
    }
    static QHash<HWINEVENTHOOK, WindowsLiveWindow*>& destroyHooks() {
        static QHash<HWINEVENTHOOK, WindowsLiveWindow*> hooks;
        return hooks;
    }
    static void CALLBACK sourceDestroyed(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object,
                                         LONG child, DWORD, DWORD) {
        auto* self = destroyHooks().value(hook, nullptr);
        if (!self || window != hwnd(self->m_target.nativeId) || object != OBJID_WINDOW ||
            child != CHILDID_SELF)
            return;
        if (event == EVENT_OBJECT_DESTROY)
            self->m_closed = true;
        const auto changed = self->changed;
        if (changed)
            changed();
    }
    bool fail(QString* error) {
        if (error)
            *error = nativeText(QT_TRANSLATE_NOOP(
                "LiveWindowSource", "The live window display could not be initialized."));
        stopPresentation();
        return false;
    }
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<WindowsLiveWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<WindowsLiveWindow*>(
                reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            HDC buffered = nullptr;
            BP_PAINTPARAMS parameters{};
            parameters.cbSize = sizeof(parameters);
            const HPAINTBUFFER buffer =
                BeginBufferedPaint(dc, &bounds, BPBF_TOPDOWNDIB, &parameters, &buffered);
            const HBRUSH brush = CreateSolidBrush(RGB(
                self->m_background.red(), self->m_background.green(), self->m_background.blue()));
            FillRect(buffered ? buffered : dc, &bounds, brush);
            DeleteObject(brush);
            if (buffer) {
                BufferedPaintSetAlpha(buffer, &bounds, 255);
                EndBufferedPaint(buffer, TRUE);
            }
            EndPaint(window, &paint);
            return 0;
        }
        if (self && self->m_shell &&
            (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
             message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
             message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
             message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
             message == WM_XBUTTONDBLCLK || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL ||
             message == WM_CONTEXTMENU)) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL &&
                message != WM_CONTEXTMENU) {
                ClientToScreen(window, &point);
                ScreenToClient(self->m_shell, &point);
                lParam = MAKELPARAM(point.x, point.y);
            }
            return SendMessageW(self->m_shell, message, wParam, lParam);
        }
        if (message == WM_ERASEBKGND)
            return 1;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    LiveWindowTarget m_target;
    QPointer<QWidget> m_owner;
    HWND m_host = nullptr;
    HWND m_shell = nullptr;
    HWND m_originalOwner = nullptr;
    HTHUMBNAIL m_thumbnail = nullptr;
    HWINEVENTHOOK m_destroyHook = nullptr;
    HWINEVENTHOOK m_locationHook = nullptr;
    QColor m_background = Qt::white;
    bool m_presenting = false;
    bool m_closed = false;
};
} // namespace

std::optional<LiveWindowTarget> liveWindowTargetAtPoint(const QPoint& point,
                                                        const QVector<quintptr>& excluded) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (excluded.contains(reinterpret_cast<quintptr>(window)) || !validWindow(window))
            continue;
        const QRect bounds = desktopBounds(window);
        if (!bounds.contains(point))
            continue;
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        return LiveWindowTarget{reinterpret_cast<quintptr>(window), process, bounds, point};
    }
    return std::nullopt;
}
namespace live_window_detail {
std::unique_ptr<NativeWindow> createNativeWindow(const LiveWindowTarget& target) {
    return std::make_unique<WindowsLiveWindow>(target);
}
} // namespace live_window_detail
} // namespace snow_shot::presentation
