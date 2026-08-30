#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QRegion>
#include <QSize>
#include <QVector>

struct ScreenshotImageSource;
class ScreenshotOcrPresentation;

using ScreenshotOcrForegrounds = QVector<QColor>;

[[nodiscard]] QRegion screenshotOcrFilterRegion(const ScreenshotOcrPresentation& presentation,
                                                const QRectF& canvasRect,
                                                const QSize& pixelSize);

[[nodiscard]] ScreenshotOcrForegrounds resolveScreenshotOcrForegrounds(
    const QImage& source, const QRectF& canvasRect,
    const ScreenshotOcrPresentation& presentation);

[[nodiscard]] QImage renderScreenshotOcrFilteredImage(
    const QImage& source, const QRectF& canvasRect,
    const ScreenshotOcrPresentation& presentation, qreal devicePixelRatio = 1.0);

[[nodiscard]] QImage renderScreenshotOcrFilteredSource(
    const ScreenshotImageSource& source, const QRectF& canvasRect, const QSize& pixelSize,
    const ScreenshotOcrPresentation& presentation);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H
