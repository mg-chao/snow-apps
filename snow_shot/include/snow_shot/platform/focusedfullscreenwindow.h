#ifndef SNOW_SHOT_PLATFORM_FOCUSEDFULLSCREENWINDOW_H
#define SNOW_SHOT_PLATFORM_FOCUSEDFULLSCREENWINDOW_H

#include <QRectF>
#include <QVector>

class QScreen;

namespace snow_shot::platform {

struct FocusedWindowSnapshot {
    qint64 ownerProcessId = 0;
    int layer = 0;
    qreal alpha = 0.0;
    QRectF bounds;
};

// Only windows from another process may trigger fullscreen suppression.
[[nodiscard]] bool isExternalFocusedWindowProcess(qint64 processId);

[[nodiscard]] bool focusedWindowCoversDisplay(qint64 frontmostProcessId,
                                              const QVector<FocusedWindowSnapshot>& orderedWindows,
                                              const QVector<QRectF>& displayBounds);

// Permission-free best-effort checks for external focused fullscreen windows.
[[nodiscard]] bool focusedFullscreenWindowExists();
[[nodiscard]] bool focusedFullscreenGameWindowExists();
[[nodiscard]] bool focusedFullscreenWindowOnScreen(QScreen* screen);

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_FOCUSEDFULLSCREENWINDOW_H
