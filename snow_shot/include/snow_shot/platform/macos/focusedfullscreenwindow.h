#ifndef SNOW_SHOT_PLATFORM_MACOS_FOCUSEDFULLSCREENWINDOW_H
#define SNOW_SHOT_PLATFORM_MACOS_FOCUSEDFULLSCREENWINDOW_H

#include <QList>
#include <QRectF>

namespace snow_shot::platform::macos {

struct VisibleWindow {
    qint64 processId = 0;
    int layer = 0;
    QRectF bounds;
};

// Windows and display bounds use the same Quartz desktop coordinate space. The window
// list is ordered from front to back; a covered fullscreen window must not suppress keys.
[[nodiscard]] bool focusedWindowFillsDisplay(qint64 foregroundProcessId, qint64 ownProcessId,
                                             const QList<VisibleWindow>& windows,
                                             const QList<QRectF>& displays);
[[nodiscard]] bool focusedFullscreenWindowExists();

} // namespace snow_shot::platform::macos

#endif
