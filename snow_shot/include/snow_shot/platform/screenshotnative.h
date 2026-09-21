#ifndef SNOW_SHOT_PLATFORM_SCREENSHOTNATIVE_H
#define SNOW_SHOT_PLATFORM_SCREENSHOTNATIVE_H
#include "snow_shot/platform/windows/scrollinput.h"
class QWidget;
class QRegion;
namespace snow_shot::platform {
using ScrollInputResult = windows::ScrollInputResult;
#ifdef Q_OS_MACOS
void configureScreenshotOverlayWindow(QWidget* widget);
void configureScreenshotRecognitionWindow(QWidget* widget);
// Cocoa masks clip drawing, but do not route input to windows underneath.
void setScreenshotInputPassThroughRegion(QWidget* widget, const QRegion& region);
void configureScreenshotToolbarWindow(QWidget* widget);
quint32 screenshotDisplayAtCursor();
quint32 screenshotFocusedWindow();
bool screenshotScrollPermission();
ScrollInputResult sendScreenshotScroll(const QRect& desktopSelection, const QPoint& delta);
#else
inline bool screenshotScrollPermission() {
    return true;
}
inline ScrollInputResult sendScreenshotScroll(const QRect& desktopSelection, const QPoint& delta) {
    return windows::sendScrollingWheelStep(desktopSelection, delta);
}
#endif
} // namespace snow_shot::platform
#endif
