#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGESOURCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGESOURCE_H

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSize>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

class QObject;

using ScreenshotImageLoadCallback = std::function<void(QImage)>;
using ScreenshotImageLoader =
    std::function<void(QObject* receiver, ScreenshotImageLoadCallback callback)>;

struct ScreenshotImageLayer {
    QImage image;
    QRectF imageCanvasRect;
    QRectF destinationCanvasRect;
    // Visual overlays such as the captured cursor are not reconstruction donors.
    bool smartEraseSource = true;

    [[nodiscard]] bool isValid() const {
        return !image.isNull() && imageCanvasRect.isValid() && !imageCanvasRect.isEmpty() &&
               destinationCanvasRect.isValid() && !destinationCanvasRect.isEmpty() &&
               imageCanvasRect.contains(destinationCanvasRect);
    }
};

struct ScreenshotImageSource {
    struct OpacityMetadata {
        struct Entry {
            qint64 imageKey = 0;
            bool opaque = false;
        };
        std::mutex mutex;
        QList<Entry> entries;
    };

    QImage materializedImage;
    QRectF materializedCanvasRect;
    QList<ScreenshotImageLayer> layers;
    // Copies sent to different overlays share the derived answer, while QImage's
    // cache key prevents a detached or edited image from reusing stale opacity.
    std::shared_ptr<OpacityMetadata> opacityMetadata;

    [[nodiscard]] bool imageIsOpaque(const QImage& image) const;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] qsizetype opacityMetadataEntryCountForTesting() const {
        if (opacityMetadata == nullptr)
            return 0;
        const std::lock_guard lock(opacityMetadata->mutex);
        return opacityMetadata->entries.size();
    }
#endif

    [[nodiscard]] static ScreenshotImageSource fromImage(QImage image, const QRectF& canvasRect) {
        ScreenshotImageSource source;
        source.materializedImage = std::move(image);
        source.materializedCanvasRect = canvasRect.normalized();
        source.opacityMetadata = std::make_shared<OpacityMetadata>();
        return source;
    }

    [[nodiscard]] static ScreenshotImageSource fromLayers(QList<ScreenshotImageLayer> layers) {
        ScreenshotImageSource source;
        source.layers = std::move(layers);
        source.opacityMetadata = std::make_shared<OpacityMetadata>();
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

    [[nodiscard]] bool isValid() const {
        return isMaterialized() || isLayered();
    }
};

[[nodiscard]] QImage materializeScreenshotImageSource(const ScreenshotImageSource& source,
                                                      const QRectF& canvasRect,
                                                      const QSize& pixelSize);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGESOURCE_H
