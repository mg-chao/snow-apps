#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGESOURCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGESOURCE_H

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSize>

#include <functional>
#include <utility>

class QObject;

using ScreenshotImageLoadCallback = std::function<void(QImage)>;
using ScreenshotImageLoader =
    std::function<void(QObject* receiver, ScreenshotImageLoadCallback callback)>;

struct ScreenshotImageLayer {
    QImage image;
    QRectF imageCanvasRect;
    QRectF destinationCanvasRect;

    [[nodiscard]] bool isValid() const {
        return !image.isNull() && imageCanvasRect.isValid() && !imageCanvasRect.isEmpty() &&
               destinationCanvasRect.isValid() && !destinationCanvasRect.isEmpty() &&
               imageCanvasRect.contains(destinationCanvasRect);
    }
};

struct ScreenshotImageSource {
    QImage materializedImage;
    QRectF materializedCanvasRect;
    QList<ScreenshotImageLayer> layers;

    [[nodiscard]] static ScreenshotImageSource fromImage(QImage image,
                                                         const QRectF& canvasRect) {
        ScreenshotImageSource source;
        source.materializedImage = std::move(image);
        source.materializedCanvasRect = canvasRect.normalized();
        return source;
    }

    [[nodiscard]] static ScreenshotImageSource fromLayers(QList<ScreenshotImageLayer> layers) {
        ScreenshotImageSource source;
        source.layers = std::move(layers);
        return source;
    }

    [[nodiscard]] bool isMaterialized() const {
        return !materializedImage.isNull() && materializedCanvasRect.isValid() &&
               !materializedCanvasRect.isEmpty();
    }

    [[nodiscard]] bool isLayered() const {
        if (layers.isEmpty()) {
            return false;
        }
        for (const ScreenshotImageLayer& layer : layers) {
            if (!layer.isValid()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool isValid() const { return isMaterialized() || isLayered(); }

    [[nodiscard]] qsizetype retainedBytes() const {
        qsizetype bytes = materializedImage.sizeInBytes();
        for (const ScreenshotImageLayer& layer : layers) {
            bytes += layer.image.sizeInBytes();
        }
        return bytes;
    }
};

[[nodiscard]] QImage materializeScreenshotImageSource(const ScreenshotImageSource& source,
                                                      const QRectF& canvasRect,
                                                      const QSize& pixelSize);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGESOURCE_H
