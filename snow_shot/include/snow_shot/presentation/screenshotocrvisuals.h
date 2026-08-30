#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QRegion>
#include <QSize>

struct ScreenshotImageSource;
class ScreenshotOcrPresentation;

[[nodiscard]] QRegion screenshotOcrFilterRegion(const ScreenshotOcrPresentation& presentation,
                                                const QRectF& canvasRect,
                                                const QSize& pixelSize);

[[nodiscard]] QImage renderScreenshotOcrFilteredImage(
    const QImage& source, const QRectF& canvasRect,
    const ScreenshotOcrPresentation& presentation, const QColor& backgroundColor,
    qreal devicePixelRatio = 1.0);

[[nodiscard]] QImage renderScreenshotOcrFilteredSource(
    const ScreenshotImageSource& source, const QRectF& canvasRect, const QSize& pixelSize,
    const ScreenshotOcrPresentation& presentation, const QColor& backgroundColor);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H
