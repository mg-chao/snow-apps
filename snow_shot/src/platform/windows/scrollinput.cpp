#include "snow_shot/platform/windows/scrollinput.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace snow_shot::platform::windows {

ScrollInputResult sendScrollingWheelStep(const QRect& physicalSelection, const QPoint& wheelDelta) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (physicalSelection.isEmpty() || wheelDelta.isNull()) {
        return {ScrollInputResult::Status::InvalidRequest, 0};
    }
    const QPoint center = physicalSelection.center();
    const POINT screenPoint{center.x(), center.y()};
    // Find the application beneath our capture overlays without moving the user's cursor.
    HWND target = nullptr;
    for (HWND window = GetTopWindow(nullptr); window != nullptr;
         window = GetWindow(window, GW_HWNDNEXT)) {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        RECT bounds{};
        if (processId != GetCurrentProcessId() && IsWindowVisible(window) &&
            IsWindowEnabled(window) && GetWindowRect(window, &bounds) &&
            PtInRect(&bounds, screenPoint)) {
            target = window;
            break;
        }
    }
    if (target == nullptr) {
        return {ScrollInputResult::Status::TargetNotFound, 0};
    }
    for (;;) {
        POINT clientPoint = screenPoint;
        if (!ScreenToClient(target, &clientPoint)) {
            return {ScrollInputResult::Status::CoordinateFailure, GetLastError()};
        }
        const HWND child = ChildWindowFromPointEx(
            target, clientPoint, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (child == nullptr || child == target) {
            break;
        }
        target = child;
    }
    const bool horizontal = wheelDelta.x() != 0;
    const int delta = horizontal ? wheelDelta.x() : wheelDelta.y();
    if (!PostMessageW(target, horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL,
                      MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(center.x(), center.y())))
        return {ScrollInputResult::Status::PostFailed, GetLastError()};
    return {ScrollInputResult::Status::Posted, 0};
#else
    Q_UNUSED(physicalSelection);
    Q_UNUSED(wheelDelta);
    return {};
#endif
}

} // namespace snow_shot::platform::windows
