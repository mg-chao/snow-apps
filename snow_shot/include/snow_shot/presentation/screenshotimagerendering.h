#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGERENDERING_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGERENDERING_H

class QPainter;
class QImage;
class QRectF;
class QRegion;

// Draw only exposed source slices, rebasing large image coordinates when required
// by the raster paint engine. Source pixels remain shared wherever possible.
void paintExposedScreenshotImage(QPainter& painter, const QRectF& targetRect, const QImage& image,
                                 const QRectF& sourceRect, const QRegion& exposedRegion);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGERENDERING_H
