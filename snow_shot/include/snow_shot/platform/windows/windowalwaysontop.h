#ifndef SNOW_SHOT_PLATFORM_WINDOWS_WINDOWALWAYSONTOP_H
#define SNOW_SHOT_PLATFORM_WINDOWS_WINDOWALWAYSONTOP_H

#include <QPoint>
#include <QString>
#include <QtGui/qwindowdefs.h>

#include <functional>
#include <optional>

namespace snow_shot::platform::windows {

// An accepted native update may be queued on the target window's thread.
enum class WindowAlwaysOnTopResult { ChangeRequested, Ignored, Failed };

namespace detail {
[[nodiscard]] bool isWindowAlwaysOnTopTarget(const QString& className, Qt::WindowType windowType);
}

// Native operations are injectable so targeting and failure paths can run offscreen.
struct WindowAlwaysOnTopAccess {
    std::function<std::optional<QPoint>()> cursorPosition;
    std::function<WId(const QPoint&)> windowAt;
    std::function<WId(WId)> rootWindow;
    std::function<bool(WId)> eligible;
    std::function<std::optional<bool>(WId)> alwaysOnTop;
    std::function<bool(WId, bool, quint32)> setAlwaysOnTop;
};

// Runs before native state is read so a stateful owner can resolve auxiliary
// surfaces and toggle its own state. nullopt delegates to the native setter;
// false reports failure without bypassing the owner with a native fallback.
using LocalWindowAlwaysOnTopHandler = std::function<std::optional<bool>(WId)>;

[[nodiscard]] WindowAlwaysOnTopResult
toggleWindowUnderCursorAlwaysOnTop(const WindowAlwaysOnTopAccess& access,
                                   const LocalWindowAlwaysOnTopHandler& localHandler = {});
[[nodiscard]] WindowAlwaysOnTopResult
toggleWindowUnderCursorAlwaysOnTop(const LocalWindowAlwaysOnTopHandler& localHandler = {});

} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_WINDOWALWAYSONTOP_H
