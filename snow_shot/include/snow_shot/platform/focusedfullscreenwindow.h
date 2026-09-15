#ifndef SNOW_SHOT_PLATFORM_FOCUSEDFULLSCREENWINDOW_H
#define SNOW_SHOT_PLATFORM_FOCUSEDFULLSCREENWINDOW_H

#include <QRectF>
#include <QVector>

namespace snow_shot::platform {

struct FocusedWindowSnapshot {
    qint64 ownerProcessId = 0;
    int layer = 0;
    qreal alpha = 0.0;
    QRectF bounds;
};

[[nodiscard]] bool focusedWindowCoversDisplay(qint64 frontmostProcessId,
                                              const QVector<FocusedWindowSnapshot>& orderedWindows,
                                              const QVector<QRectF>& displayBounds);

// Permission-free best-effort check used only to suppress configured global shortcuts.
[[nodiscard]] bool focusedFullscreenWindowExists();

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_FOCUSEDFULLSCREENWINDOW_H
