#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H

#include "snow_draw_engine_qt/snow_canvas_region_filter.h"

#include <QColor>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QSize>
#include <atomic>

struct ScreenshotImageSource;
class ScreenshotOcrPresentation;

// Bounded interior/perimeter sampling selects an opaque dominant color, including split
// backgrounds. Disabling fill selects the separate Blur policy and clears background estimates.
void prepareScreenshotOcrFillColors(ScreenshotOcrPresentation& presentation, const QImage& source,
                                    const QRectF& canvasRect, bool backgroundFill);

// Sample original, unfiltered pixels once per image/geometry. Independent of Background Fill.
// Invalid estimates retain the renderer's contrast fallback. Cancellation never caches partial
// work. Normal recognition prepares these on its local worker before publishing the presentation.
void prepareScreenshotOcrTextColors(ScreenshotOcrPresentation& presentation, const QImage& source,
                                    const QRectF& canvasRect,
                                    const std::atomic_bool* cancelled = nullptr);

[[nodiscard]] QRegion screenshotOcrFilterRegion(const ScreenshotOcrPresentation& presentation,
                                                const QRectF& canvasRect, const QSize& pixelSize);

// Maps an image-pixel crop reported by renderScreenshotOcrFilteredImage back to
// canvas coordinates using the linear canvasRect-to-pixel mapping.
[[nodiscard]] QRectF screenshotOcrFilteredImageCanvasRect(const QRectF& canvasRect,
                                                          const QSize& pixelSize,
                                                          const QRect& filteredPixels);

// Background Fill always paints opaque colors; missing estimates use the supplied background or
// white. Explicit Blur retains blur/tint. Prepared solid colors paint in line order. The result is
// a cropped sub-image covering only the text regions plus the blur support margin; filteredPixels
// (image pixel coordinates within source) reports that crop. When no text region exists the source
// itself is returned and the crop is the whole image. scratch, when provided, pools the engine's
// reduced-resolution buffers across calls.
[[nodiscard]] QImage renderScreenshotOcrFilteredImage(
    const QImage& source, const QRectF& canvasRect, const ScreenshotOcrPresentation& presentation,
    const QColor& backgroundColor, qreal devicePixelRatio = 1.0, QRect* filteredPixels = nullptr,
    SnowCanvasRegionFilterScratch* scratch = nullptr);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRVISUALS_H
