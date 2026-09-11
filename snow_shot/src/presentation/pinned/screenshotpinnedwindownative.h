#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H

#include <QRect>
#include <QKeyCombination>
#include <QList>
#include <QWidget>
#include <Qt>

#include <memory>

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
[[nodiscard]] bool applySystemResizeStyle(WId windowId);
[[nodiscard]] bool activateWindow(WId windowId);
#ifdef Q_OS_MACOS
[[nodiscard]] bool leftMouseButtonPressed();
#endif
[[nodiscard]] bool installSynchronizedResize(WId windowId, const bool* interactiveResizeActive);
void removeSynchronizedResize(WId windowId);
[[nodiscard]] bool applyCursor(Qt::CursorShape shape);
[[nodiscard]] bool synchronizeClientPaint(
    WId windowId, PaintSynchronization synchronization = PaintSynchronization::InvalidateAndUpdate);
} // namespace screenshot_pinned_window_native

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOWNATIVE_H
