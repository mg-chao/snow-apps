#ifndef SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCURSORREFRESH_H
#define SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCURSORREFRESH_H

#include <QtTypes>

namespace snow_shot::platform::windows::detail {
// WinEvent's source thread is the producer, which may be DWM rather than the target app.
[[nodiscard]] bool isUnderlyingCursorUpdate(quint32 event, qint32 object, quint32 sourceThread,
                                            quint32 callerThread, quint32 targetThread);
} // namespace snow_shot::platform::windows::detail

#endif // SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCURSORREFRESH_H
