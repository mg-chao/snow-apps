#ifndef SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H

#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>

namespace snow_shot::presentation::recording {
struct ScreenRecordingAreaFrameGeometry {
    QRect windowGeometry;
    QRectF frameRect;
    QRectF selectionRect;
    qreal borderWidth = 1.0;
};

[[nodiscard]] ScreenRecordingAreaFrameGeometry
screenRecordingAreaFrameGeometry(const QRectF& logicalRegion, qreal physicalScale);

[[nodiscard]] QRect screenRecordingCompatibleCaptureRegion(const QRect& selectedPhysicalRegion,
                                                          const QRect& physicalBounds);

[[nodiscard]] QSize screenRecordingMaximumSizeForClarity(const QString& clarity);

[[nodiscard]] QSize screenRecordingOrientedMaximumSize(const QSize& maximumSize,
                                                      const QSize& captureSize);
} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGGEOMETRY_H
