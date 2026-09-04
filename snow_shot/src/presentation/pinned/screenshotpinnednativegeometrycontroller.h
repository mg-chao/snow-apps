#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDNATIVEGEOMETRYCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDNATIVEGEOMETRYCONTROLLER_H

#include "screenshotpinnedresizegeometry.h"

#include <QPoint>
#include <QRect>
#include <QSize>

#include <optional>

class ScreenshotPinnedNativeGeometryController final {
  public:
    enum class Phase {
        Uninitialized,
        Stable,
        MovePending,
        Moving,
        ResizePending,
        Resizing,
        DpiChanging,
        Programmatic,
        Closing,
    };

    enum class Origin {
        InitialPlacement,
        UserMove,
        UserResize,
        DpiTransition,
        Scale,
        ImageTransform,
        Thumbnail,
        Animation,
        Restoration,
    };

    struct GeometryChange {
        QRect previousGeometry;
        QRect geometry;
        bool positionChanged = false;
        bool sizeChanged = false;
        bool dpiChanged = false;

        [[nodiscard]] bool isValid() const {
            return geometry.isValid() && !geometry.isEmpty();
        }
    };

    [[nodiscard]] bool initialize(const QRect& geometry);
    void beginClosing();

    [[nodiscard]] Phase phase() const;
    [[nodiscard]] QRect committedGeometry() const;
    [[nodiscard]] QRect targetGeometry() const;
    [[nodiscard]] bool hasInteractiveTransaction() const;
    [[nodiscard]] bool hasAcceptedInteractiveGeometry() const;

    [[nodiscard]] bool beginMove(const QPoint& nativeCursorPosition);
    [[nodiscard]] bool beginResize(screenshot_pinned_resize_geometry::DragHandle handle);
    void cancelPendingInteraction();

    [[nodiscard]] QRect constrainWindowPos(const QRect& proposed, bool moveRequested,
                                           bool sizeRequested) const;
    [[nodiscard]] QRect updateMove(const QRect& proposed, const QPoint& nativeCursorPosition);
    [[nodiscard]] std::optional<QRect>
    updateResize(const QRect& proposed, screenshot_pinned_resize_geometry::DragHandle handle,
                 const QSize& baseline, double minimumScale, double maximumScale);
    // Accepts a system-proposed DPI transition geometry verbatim. The window
    // never overrides the position or size the monitor change produced; it
    // only records the transition so the settled physical size can re-derive
    // the scale value.
    [[nodiscard]] bool adoptDpiTarget(const QRect& suggested);

    [[nodiscard]] bool beginProgrammatic(const QRect& target, Origin origin);
    [[nodiscard]] QRect finishInteractiveTarget() const;
    [[nodiscard]] GeometryChange commitTarget(bool transactionFinished = true);
    void prepareRollback();
    [[nodiscard]] GeometryChange finishRollback();

  private:
    [[nodiscard]] bool beginInteractive(Phase phase, Origin origin);
    void resetTransaction();

    QRect m_committedGeometry;
    QRect m_transactionStartGeometry;
    QRect m_targetGeometry;
    QPoint m_moveStartCursor;
    screenshot_pinned_resize_geometry::DragHandle m_resizeHandle =
        screenshot_pinned_resize_geometry::DragHandle::BottomRight;
    Phase m_phase = Phase::Uninitialized;
    Origin m_origin = Origin::InitialPlacement;
    bool m_acceptedInteractiveGeometry = false;
    bool m_dpiChanged = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDNATIVEGEOMETRYCONTROLLER_H
