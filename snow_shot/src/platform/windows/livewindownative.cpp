#include "../../presentation/pinned/livewindownative_p.h"
#include "livewindowpresentation.h"

#include <QCoreApplication>
#include <QHash>
#include <qt_windows.h>
#include <dwmapi.h>

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

class WindowsLiveWindow final : public live_window_detail::NativeWindowMonitor {
  public:
    explicit WindowsLiveWindow(LiveWindowTarget target) : m_target(std::move(target)) {}
    ~WindowsLiveWindow() override {
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
        m_minimizeHook = SetWinEventHook(
            EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, sourceDestroyed,
            static_cast<DWORD>(m_target.processId), 0, WINEVENT_OUTOFCONTEXT);
        if (m_minimizeHook)
            destroyHooks().insert(m_minimizeHook, this);
        return true;
    }
    bool hasLifecycleNotifications() const override {
        return m_destroyHook && m_locationHook && m_minimizeHook;
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

  private:
    void stopMonitoring() {
        for (auto* hook : {&m_destroyHook, &m_locationHook, &m_minimizeHook}) {
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
        if (!self || window != hwnd(self->m_target.nativeId))
            return;
        if (event != EVENT_SYSTEM_MINIMIZESTART && event != EVENT_SYSTEM_MINIMIZEEND &&
            (object != OBJID_WINDOW || child != CHILDID_SELF))
            return;
        if (event == EVENT_OBJECT_DESTROY)
            self->m_closed = true;
        const auto changed = self->changed;
        if (changed)
            changed();
    }
    LiveWindowTarget m_target;
    HWINEVENTHOOK m_destroyHook = nullptr;
    HWINEVENTHOOK m_locationHook = nullptr;
    HWINEVENTHOOK m_minimizeHook = nullptr;
    bool m_closed = false;
};

class WindowsLivePresentation final : public live_window_detail::NativeWindowPresentation {
  public:
    explicit WindowsLivePresentation(const LiveWindowTarget& target)
        : m_surface(hwnd(target.nativeId)) {}
    bool present(const LiveWindowPresentation& presentation, const QRectF& crop,
                 const QRect& sourceBounds, QString* error) override {
        if (m_surface.present(presentation, crop, sourceBounds))
            return true;
        if (error)
            *error = nativeText(QT_TRANSLATE_NOOP(
                "LiveWindowSource", "The live window display could not be initialized."));
        return false;
    }
    bool presenting() const override {
        return m_surface.presenting();
    }
    void stopPresentation() override {
        m_surface.stop();
    }

  private:
    LiveWindowPresentationSurface m_surface;
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
        QString title(GetWindowTextLengthW(window) + 1, Qt::Uninitialized);
        const int length = GetWindowTextW(window, reinterpret_cast<wchar_t*>(title.data()),
                                          static_cast<int>(title.size()));
        title.truncate(length);
        return LiveWindowTarget{reinterpret_cast<quintptr>(window), process, bounds, point, title};
    }
    return std::nullopt;
}
namespace live_window_detail {
std::unique_ptr<NativeWindowCapture> createNativeWindowCapture(const LiveWindowTarget&) {
    return {};
}
std::unique_ptr<NativeWindowMonitor> createNativeWindowMonitor(const LiveWindowTarget& target) {
    return std::make_unique<WindowsLiveWindow>(target);
}
std::unique_ptr<NativeWindowPresentation>
createNativeWindowPresentation(const LiveWindowTarget& target) {
    return std::make_unique<WindowsLivePresentation>(target);
}
} // namespace live_window_detail
} // namespace snow_shot::presentation
