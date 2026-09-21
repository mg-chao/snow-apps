#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWPLATFORM_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWPLATFORM_H

#include "pinnedplacementgeometry.h"
#include <QObject>
#include <QKeyCombination>
#include <QPointer>
#include <QRectF>
#include <QScreen>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

class ScreenshotPinnedWindow;
class PinnedWindowWindowsEvents {
  public:
    static bool handle(ScreenshotPinnedWindow& window, const QByteArray& type, void* message,
                       qintptr* result);
};

namespace snow_shot::presentation {
using PinnedPlacement = storage::PinnedWindowPlacement;

[[nodiscard]] PinnedDisplayGeometry pinnedDisplayGeometry(const QScreen& screen);

// The integer geometry controller operates in pixels of an explicitly supplied
// display. These boundary conversions must never be used to select a display.
[[nodiscard]] PinnedPlacement pinnedPlacement(const QRect& pixels, const QScreen& screen);
[[nodiscard]] QRect pinnedPixelRect(const PinnedPlacement& placement, const QScreen& screen);
[[nodiscard]] QRectF pinnedDesktopRect(const PinnedPlacement& placement, const QScreen& screen);
[[nodiscard]] QScreen* pinnedDisplay(const PinnedPlacement& placement, QScreen* fallback = nullptr);
[[nodiscard]] QScreen* pinnedDisplayAt(const QPointF& desktopPosition);
[[nodiscard]] PinnedPlacement recoverPinnedPlacement(PinnedPlacement placement,
                                                     const QScreen& screen);

class PinnedWindowPlatform : public QObject {
  public:
    enum class Role { Image, Auxiliary };
    enum class GeometryUpdate { PreserveContents, DiscardContents };
    explicit PinnedWindowPlatform(QWidget* window, Role role);
    ~PinnedWindowPlatform() override;
    [[nodiscard]] virtual bool attach() = 0;
    virtual void detach() = 0;
    [[nodiscard]] virtual bool
    applyPlacement(const PinnedPlacement& placement, QScreen* screen,
                   GeometryUpdate update = GeometryUpdate::PreserveContents) = 0;
    [[nodiscard]] virtual std::optional<PinnedPlacement> placement() const = 0;
    [[nodiscard]] virtual bool setInputTransparent(bool transparent) = 0;
    [[nodiscard]] virtual bool setStaysOnTop(bool) {
        return false;
    }
    [[nodiscard]] virtual bool activate() = 0;
    virtual bool handleNativeEvent(const QByteArray&, void*, qintptr*) {
        return false;
    }
    virtual void setResizeInteractionState(const bool*) {}
    virtual void setMoveKeyCombinations(const QList<QKeyCombination>&) {}
    virtual void setSystemMoveActive(bool) {}
    [[nodiscard]] virtual bool startSystemMove() {
        return false;
    }
    [[nodiscard]] virtual bool systemInteractionReleased() const {
        return false;
    }
    [[nodiscard]] virtual QRect framePixelGeometry() const {
        return pixelGeometry();
    }
    [[nodiscard]] virtual bool synchronizePaint(bool) {
        return true;
    }
    [[nodiscard]] virtual std::optional<QPointF> pointerPosition() const;
    [[nodiscard]] virtual bool usesControlledInteraction() const {
        return false;
    }
    [[nodiscard]] virtual std::optional<bool> pointerInside() const;
    // Reconcile an AppKit backing-display change while retaining the requested
    // pixel extent and desktop top-left. Interactive dragging uses its own anchor.
    [[nodiscard]] bool
    applyStablePlacement(PinnedPlacement placement, QScreen* screen,
                         GeometryUpdate update = GeometryUpdate::PreserveContents);
    [[nodiscard]] virtual bool
    applyPixelGeometry(const QRect& pixels, QScreen* screen,
                       GeometryUpdate update = GeometryUpdate::PreserveContents);
    [[nodiscard]] virtual QRect pixelGeometry() const;
    std::function<void()> environmentChanged;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void observeNativeSurface();
    QPointer<QWidget> m_window;
    QPointer<QWindow> m_surface;
    Role m_role;
    bool m_transparent = false;
};
[[nodiscard]] std::unique_ptr<PinnedWindowPlatform>
createPinnedWindowPlatform(QWidget* window,
                           PinnedWindowPlatform::Role role = PinnedWindowPlatform::Role::Image);
// Owned by the auxiliary QWidget. Reattaches on every native-surface recreation.
PinnedWindowPlatform* configurePinnedAuxiliary(QWidget* window);
} // namespace snow_shot::presentation
#endif
