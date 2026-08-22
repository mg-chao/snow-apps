#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H

#include <QRect>
#include <QWidget>
#include <Qt>

namespace screenshot_pinned_window_native {
enum class GeometryUpdate {
    PreserveClientPixels,
    DiscardClientPixels,
};

[[nodiscard]] Qt::WindowFlags windowFlags();
[[nodiscard]] bool
applyClientGeometry(WId windowId, const QRect& geometry,
                    GeometryUpdate update = GeometryUpdate::PreserveClientPixels);
[[nodiscard]] QRect currentClientGeometry(WId windowId);
[[nodiscard]] bool applySystemResizeStyle(WId windowId);
[[nodiscard]] bool installSynchronizedResize(WId windowId, const bool* interactiveResizeActive);
void removeSynchronizedResize(WId windowId);
[[nodiscard]] bool applyCursor(Qt::CursorShape shape);
[[nodiscard]] bool synchronizeClientPaint(WId windowId);
[[nodiscard]] bool beginWindowMoveCapture(WId windowId);
void endWindowMoveCapture(WId windowId);
} // namespace screenshot_pinned_window_native

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H
