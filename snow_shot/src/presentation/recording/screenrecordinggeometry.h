#ifndef SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H

#include <QRect>
#include <QMargins>
#include <QRectF>
#include <QSize>
#include <QString>

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

struct ScreenRecordingObservedGeometry {
    QRect physicalRegion;
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

[[nodiscard]] ScreenRecordingAreaBorderGeometry
screenRecordingAreaBorderGeometry(const QRectF& frameRect, const QRectF& selectionRect,
                                  qreal paddingWidth);

[[nodiscard]] QRect screenRecordingCompatibleCaptureRegion(const QRect& selectedPhysicalRegion,
                                                           const QRect& physicalBounds);

[[nodiscard]] QSize screenRecordingMaximumSizeForClarity(const QString& clarity);

[[nodiscard]] QSize screenRecordingOrientedMaximumSize(const QSize& maximumSize,
                                                       const QSize& captureSize);
} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H
