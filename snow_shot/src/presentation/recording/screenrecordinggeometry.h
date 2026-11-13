#ifndef SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H

#include <QRect>
#include <QMargins>
#include <QRectF>
#include <QSize>
#include <QString>

#include <chrono>

namespace snow_shot::presentation::recording {
struct ScreenRecordingAreaFrameGeometry {
    QRect windowGeometry;
    QRectF frameRect;
    QRectF selectionRect;
    qreal borderWidth = 1.0;
    // Transparent gap between the selected pixels and the visible frame.
    qreal paddingWidth = 0.0;
};

struct ScreenRecordingAreaBorderGeometry {
    QRectF top;
    QRectF bottom;
    QRectF left;
    QRectF right;
};

inline constexpr int screenRecordingPhysicalFrameInset = 3;

// Geometry uses physical desktop pixels on Windows and desktop points on macOS.
class ScreenRecordingFollowGuideMotion {
  public:
    ScreenRecordingFollowGuideMotion(const QSizeF& selectionSize, const QRectF& bounds,
                                     const QPointF& cursor, bool smoothMovement, bool reduceJitter);

    void track(const QPointF& cursor);
    void advance(std::chrono::duration<qreal> elapsed);
    void recenter(const QPointF& center);
    [[nodiscard]] bool isMoving() const;
    [[nodiscard]] QRectF viewport() const;

  private:
    [[nodiscard]] QPointF centeredOrigin(const QPointF& center) const;

    QRectF m_interior;
    QSizeF m_size;
    QPointF m_position;
    QPointF m_target;
    bool m_smoothMovement;
    bool m_reduceJitter;
};

[[nodiscard]] int screenRecordingMinimumExtent(qreal physicalScale = 1.0);
[[nodiscard]] QRect screenRecordingNormalizedRegion(const QRect& region, const QRect& bounds,
                                                    qreal physicalScale = 1.0);
// Selects bounds and units using the application's platform coordinate contract.
[[nodiscard]] QRect screenRecordingNormalizedRegion(const QRect& region);

struct ScreenRecordingObservedGeometry {
    QRect recordingRegion;
    QRectF frameRect;
    QRectF selectionRect;
    qreal paddingWidth = 0.0;
};

// The native client rectangle already uses virtual-desktop physical pixels.
[[nodiscard]] ScreenRecordingObservedGeometry
screenRecordingObservedGeometry(const QRect& physicalClientRect, qreal physicalScale,
                                const QMargins& physicalInsets = QMargins(3, 3, 3, 3));

[[nodiscard]] ScreenRecordingAreaFrameGeometry
screenRecordingAreaFrameGeometry(const QRectF& logicalRegion, qreal physicalScale);

// Position-independent backing-store size for a moving, presentation-only border.
[[nodiscard]] ScreenRecordingAreaFrameGeometry
screenRecordingFollowFrameGeometry(const QRectF& logicalRegion, qreal physicalScale);

[[nodiscard]] ScreenRecordingAreaBorderGeometry
screenRecordingAreaBorderGeometry(const QRectF& frameRect, const QRectF& selectionRect,
                                  qreal paddingWidth);

[[nodiscard]] QRect screenRecordingCompatibleCaptureRegion(const QRect& selectedRecordingRegion,
                                                           const QRect& physicalBounds);

[[nodiscard]] QSize screenRecordingMaximumSizeForClarity(const QString& clarity);

[[nodiscard]] QSize screenRecordingOrientedMaximumSize(const QSize& maximumSize,
                                                       const QSize& captureSize);
} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H
