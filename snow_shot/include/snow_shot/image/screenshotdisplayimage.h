#ifndef SNOW_SHOT_IMAGE_SCREENSHOTDISPLAYIMAGE_H
#define SNOW_SHOT_IMAGE_SCREENSHOTDISPLAYIMAGE_H

#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QColorSpace>
#include <QImage>

namespace snow_shot::image {
// QPainter does not convert imported ICC profiles when compositing them with sRGB
// screenshots. Keep the original raster for editing/export and prepare this view
// on the decoding worker whenever the source is already available there.
[[nodiscard]] inline QImage normalizeScreenshotDisplayImage(const QImage& image) {
    if (image.isNull())
        return {};
    const QColorSpace srgb(QColorSpace::SRgb);
    QImage normalized =
        image.colorSpace().isValid() && image.colorSpace() != srgb
            ? snowCanvasColorConvertedImage(image, srgb, QImage::Format_ARGB32_Premultiplied)
            : snowCanvasConvertImage(image, QImage::Format_ARGB32_Premultiplied);
    if (normalized.devicePixelRatio() != 1.0 && !snowCanvasDetachImage(normalized))
        return {};
    normalized.setDevicePixelRatio(1.0);
    return normalized;
}
} // namespace snow_shot::image

#endif
