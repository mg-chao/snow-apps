#ifndef SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H
#define SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H

class QWidget;
class QPoint;

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtTypes>

namespace snow_shot::platform::windows {
void setupDwmShadow(QWidget* window);
void bringWindowToForeground(QWidget* window);
[[nodiscard]] bool setWindowExcludedFromCapture(QWidget* window, bool excluded);
[[nodiscard]] bool isTitleBarDragArea(QWidget* titleBar, const QPoint& globalPosition);
bool handleNativeWindowEvent(QWidget* titleBar, void* message, qintptr* result);
} // namespace snow_shot::platform::windows
#endif

#endif // SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H
