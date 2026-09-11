#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H

#include <QRect>
#include <QKeyCombination>
#include <QList>
#include <QWidget>
#include <Qt>

#include <memory>
#include <optional>

namespace screenshot_pinned_window_native {
class SystemMoveKeyboard final {
  public:
    explicit SystemMoveKeyboard(QWidget* window);
    ~SystemMoveKeyboard();
    void setKeyCombinations(const QList<QKeyCombination>& combinations);
    [[nodiscard]] bool start();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

enum class GeometryUpdate {
    PreserveClientPixels,
    DiscardClientPixels,
};

enum class PaintSynchronization {
    InvalidateAndUpdate,
    FlushAlreadyPainted,
};

[[nodiscard]] Qt::WindowFlags windowFlags();
[[nodiscard]] bool
applyClientGeometry(WId windowId, const QRect& geometry,
                    GeometryUpdate update = GeometryUpdate::PreserveClientPixels);
[[nodiscard]] QRect currentClientGeometry(WId windowId);
// The complete native window frame, which is wider than the client area on
// Windows because the pinned surface re-applies WS_THICKFRAME. Falls back to
// the client geometry when the window rect cannot be read.
[[nodiscard]] QRect currentWindowGeometry(WId windowId);
// Reports whether the live cursor is inside the window's complete native
// frame, which is wider than the client area on Windows because the pinned
// surface re-applies WS_THICKFRAME. Returns std::nullopt when the cursor or
// the window rect cannot be read so callers can fall back to event-derived
// pointer presence.
[[nodiscard]] std::optional<bool> pointerInsideWindow(WId windowId);
[[nodiscard]] bool applySystemResizeStyle(WId windowId);
[[nodiscard]] bool activateWindow(WId windowId);
[[nodiscard]] bool installSynchronizedResize(WId windowId, const bool* interactiveResizeActive);
void removeSynchronizedResize(WId windowId);
[[nodiscard]] bool applyCursor(Qt::CursorShape shape);
[[nodiscard]] bool synchronizeClientPaint(
    WId windowId, PaintSynchronization synchronization = PaintSynchronization::InvalidateAndUpdate);
} // namespace screenshot_pinned_window_native

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H
