#include "snow_shot/platform/windows/windowalwaysontop.h"

#include <QDebug>
#include <QGuiApplication>
#include <QWidget>
#include <qt_windows.h>

namespace snow_shot::platform::windows {

bool detail::isWindowAlwaysOnTopTarget(const QString& className, Qt::WindowType windowType) {
    if (windowType == Qt::Popup || windowType == Qt::ToolTip)
        return false;
    for (const auto& excluded :
         {QStringLiteral("Progman"), QStringLiteral("WorkerW"), QStringLiteral("Shell_TrayWnd"),
          QStringLiteral("Shell_SecondaryTrayWnd"), QStringLiteral("#32768"),
          QStringLiteral("tooltips_class32")}) {
        if (className.compare(excluded, Qt::CaseInsensitive) == 0)
            return false;
    }
    return !className.isEmpty();
}

namespace {

bool eligibleWindow(WId id) {
    const HWND window = reinterpret_cast<HWND>(id);
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        window == GetDesktopWindow() || window == GetShellWindow())
        return false;

    wchar_t name[256]{};
    if (GetClassNameW(window, name, 256) == 0)
        return false;
    const auto* widget = QWidget::find(id);
    return detail::isWindowAlwaysOnTopTarget(QString::fromWCharArray(name),
                                             widget ? widget->windowType() : Qt::Window);
}

WindowAlwaysOnTopAccess nativeAccess() {
    return {[]() -> std::optional<QPoint> {
                if (QGuiApplication::platformName() != QStringLiteral("windows"))
                    return std::nullopt;
                POINT point{};
                if (!GetCursorPos(&point))
                    return std::nullopt;
                return QPoint(point.x, point.y);
            },
            [](const QPoint& position) {
                return reinterpret_cast<WId>(WindowFromPoint({position.x(), position.y()}));
            },
            [](WId id) {
                return reinterpret_cast<WId>(GetAncestor(reinterpret_cast<HWND>(id), GA_ROOT));
            },
            eligibleWindow,
            [](WId id) -> std::optional<bool> {
                SetLastError(ERROR_SUCCESS);
                const LONG_PTR style = GetWindowLongPtrW(reinterpret_cast<HWND>(id), GWL_EXSTYLE);
                if (style == 0 && GetLastError() != ERROR_SUCCESS)
                    return std::nullopt;
                return (style & WS_EX_TOPMOST) != 0;
            },
            [](WId id, bool enabled, quint32 flags) {
                if (!SetWindowPos(reinterpret_cast<HWND>(id),
                                  enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags)) {
                    qWarning() << "Could not change the window's Always on Top state:"
                               << GetLastError();
                    return false;
                }
                return true;
            }};
}

} // namespace

WindowAlwaysOnTopResult
toggleWindowUnderCursorAlwaysOnTop(const WindowAlwaysOnTopAccess& access,
                                   const LocalWindowAlwaysOnTopHandler& localHandler) {
    const auto fail = [] {
        qWarning("Could not toggle Always on Top for the window under the cursor");
        return WindowAlwaysOnTopResult::Failed;
    };
    if (!access.cursorPosition || !access.windowAt || !access.rootWindow || !access.eligible)
        return fail();
    const auto position = access.cursorPosition();
    if (!position)
        return fail();
    const WId hit = access.windowAt(*position);
    if (!hit)
        return WindowAlwaysOnTopResult::Ignored;
    const WId window = access.rootWindow(hit);
    if (!window || !access.eligible(window))
        return WindowAlwaysOnTopResult::Ignored;
    if (localHandler) {
        if (const auto handled = localHandler(window))
            return *handled ? WindowAlwaysOnTopResult::ChangeRequested : fail();
    }
    if (!access.alwaysOnTop || !access.setAlwaysOnTop)
        return fail();
    const auto current = access.alwaysOnTop(window);
    if (!current)
        return fail();
    // Foreign windows need not be pumping messages. Never wait for their
    // owning thread from the application's GUI thread.
    if (!access.setAlwaysOnTop(window, !*current,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS))
        return fail();
    if (auto* widget = QWidget::find(window)) {
        const auto flags = *current ? widget->windowFlags() & ~Qt::WindowStaysOnTopHint
                                    : widget->windowFlags() | Qt::WindowStaysOnTopHint;
        widget->overrideWindowFlags(flags);
    }
    return WindowAlwaysOnTopResult::ChangeRequested;
}

WindowAlwaysOnTopResult
toggleWindowUnderCursorAlwaysOnTop(const LocalWindowAlwaysOnTopHandler& localHandler) {
    return toggleWindowUnderCursorAlwaysOnTop(nativeAccess(), localHandler);
}

} // namespace snow_shot::platform::windows
