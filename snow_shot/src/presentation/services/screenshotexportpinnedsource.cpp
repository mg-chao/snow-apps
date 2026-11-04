#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/presentation/screenshotselectioncomposition.h"
#include "snow_shot/image/screenshotlayoutcompaction.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QColorSpace>
#include <QPainter>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include <utility>

namespace {
struct PinnedPixels final {
    explicit PinnedPixels(ScreenshotPinnedViewportExportSource value) : source(std::move(value)) {}

    QMutex mutex;
    std::optional<ScreenshotPinnedViewportExportSource> source;
    QImage image;
};

QImage renderPinnedBackground(const ScreenshotPinnedViewportExportSource& source,
                              const QRectF& outputRect, const QSize& outputSize,
                              const std::function<bool()>& cancelled) {
    // An unchanged native backdrop can be read directly by the canvas exporter.
    if (!source.documentSession.isEmpty() && source.textAnnotations.empty() &&
        outputRect == source.backgroundCanvasRect && outputSize == source.backgroundImage.size())
        return source.backgroundImage;
    QImage background = SnowCanvasRuntime::renderImageSourcesToImage(
        outputRect, outputSize, {{source.backgroundImage, source.backgroundCanvasRect}});
    if (background.isNull())
        return background;
    background.setColorSpace(source.backgroundImage.colorSpace());
    if (source.textAnnotations.empty())
        return background;
    // Project first: a small crop must never detach or annotate the full source raster.
    QPainter painter(&background);
    if (!painter.isActive())
        return {};
    painter.setClipRect(background.rect());
    const QTransform toPixels(outputSize.width() / outputRect.width(), 0, 0,
                              outputSize.height() / outputRect.height(),
                              -outputRect.x() * outputSize.width() / outputRect.width(),
                              -outputRect.y() * outputSize.height() / outputRect.height());
    if (!paintScreenshotTextAnnotations(painter, source.textAnnotations, toPixels, nullptr,
                                        cancelled))
        return {};
    return background;
}
} // namespace

QImage renderScreenshotPinnedViewport(const ScreenshotPinnedViewportExportSource& source,
                                      const std::function<bool()>& cancelled) {
    const auto stopped = [&] { return cancelled && cancelled(); };
    const auto releaseCaches = qScopeGuard([&] {
        ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
        resetScreenshotLayoutCompactionCache();
        if (source.resultStyle.region)
            source.resultStyle.region->clearDerivedCache();
    });
    if (source.backgroundImage.isNull() || !source.backgroundCanvasRect.isValid() ||
        source.backgroundCanvasRect.isEmpty() || !source.contentPixelSize.isValid() ||
        source.contentPixelSize.isEmpty() || stopped()) {
        return {};
    }
    if (source.visibleCanvasRect) {
        const QRectF visible = *source.visibleCanvasRect;
        if (!visible.isValid() || visible.isEmpty())
            return {};
        const qreal sx = source.contentPixelSize.width() / source.backgroundCanvasRect.width();
        const qreal sy = source.contentPixelSize.height() / source.backgroundCanvasRect.height();
        const QSize output(std::max(1, qRound(visible.width() * sx)),
                           std::max(1, qRound(visible.height() * sy)));
        // Complex selection masks retain their full-source shape and effect origin.
        // Ordinary rectangular pins render just the requested region below.
        if (source.resultStyle.region || source.resultStyle.cornerRadius > 0 ||
            source.resultStyle.effectWidth() > 0) {
            auto full = source;
            full.visibleCanvasRect.reset();
            const QImage image = renderScreenshotPinnedViewport(full, cancelled);
            const auto layout = ScreenshotResultCompositor::layoutForContent(
                source.contentPixelSize, source.resultStyle);
            const QPoint offset(qRound((visible.x() - source.backgroundCanvasRect.x()) * sx) -
                                    layout.outputRect.x(),
                                qRound((visible.y() - source.backgroundCanvasRect.y()) * sy) -
                                    layout.outputRect.y());
            return image.isNull() ? QImage() : image.copy(QRect(offset, output));
        }
    }
    const QRectF outputRect = source.visibleCanvasRect.value_or(source.backgroundCanvasRect);
    const QSize outputSize =
        source.visibleCanvasRect
            ? QSize(std::max(1, qRound(outputRect.width() * source.contentPixelSize.width() /
                                       source.backgroundCanvasRect.width())),
                    std::max(1, qRound(outputRect.height() * source.contentPixelSize.height() /
                                       source.backgroundCanvasRect.height())))
            : source.contentPixelSize;
    // Sampling and reconstruction always retain pristine donors, independently of the backdrop.
    QList<SnowCanvasBaseImageSource> sampling = source.samplingSources;
    if (sampling.isEmpty())
        sampling.append({source.backgroundImage, source.backgroundCanvasRect});
    QList<CanvasExportSource> sources;
    sources.reserve(sampling.size());
    for (const auto& item : sampling)
        sources.append({item.image, item.canvasRect, item.coverage});
    const bool samplingMatchesBackground =
        sampling.size() == 1 &&
        sampling.front().image.cacheKey() == source.backgroundImage.cacheKey() &&
        sampling.front().canvasRect == source.backgroundCanvasRect &&
        sampling.front().coverage.isEmpty();
    const bool needsBackground = source.documentSession.isEmpty() || !samplingMatchesBackground ||
                                 source.resultStyle.region.has_value();
    QImage background;
    if (needsBackground) {
        background = renderPinnedBackground(source, outputRect, outputSize, cancelled);
        if (background.isNull() || stopped())
            return {};
    }
    QImage content;
    if (source.documentSession.isEmpty()) {
        content = std::move(background);
    } else {
        // Imported document and reconstruction pixels belong to this export.
        SnowCanvasRuntime runtime(
            SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
        if (!runtime.isValid() || !runtime.restoreDocumentSession(source.documentSession) ||
            stopped())
            return {};
        if (!runtime.prepareSmartEraseForExport(source.smartErase, sampling, outputRect, cancelled))
            return {};
        if (stopped())
            return {};
        if (source.resultStyle.region) {
            auto result = ScreenshotResultCompositor::compose(background, source.resultStyle);
            const auto layout = ScreenshotResultCompositor::layoutForContent(
                source.contentPixelSize, source.resultStyle);
            result = renderScreenshotSelectionAnnotations(runtime, outputRect, outputSize, sources,
                                                          std::move(result), layout);
            return stopped()
                       ? QImage{}
                       : ScreenshotResultCompositor::compose(result, {}, 1.0, source.outputOpacity);
        }
        // The runtime can place text marks between the pristine backdrop and drawing without
        // another output-sized raster. Effectful scenes materialize a backdrop only if needed.
        SnowCanvasExportBackgroundOverlay overlay;
        if (!needsBackground && !source.textAnnotations.empty()) {
            overlay = [&](QPainter& painter, const QTransform& mapping) {
                return paintScreenshotTextAnnotations(painter, source.textAnnotations, mapping,
                                                      nullptr, cancelled);
            };
        }
        content = runtime.renderToImage(outputRect, outputSize, sources, background, overlay);
    }
    if (stopped())
        return {};
    content.setColorSpace(source.backgroundImage.colorSpace());
    return content.isNull()
               ? QImage{}
               : ScreenshotResultCompositor::compose(
                     content,
                     source.visibleCanvasRect ? ScreenshotResultStyle{} : source.resultStyle, 1.0,
                     source.outputOpacity);
}

ScreenshotExportSource
ScreenshotExportSource::fromPinnedViewport(ScreenshotPinnedViewportExportSource source) {
    auto placement = source.clipboardPlacement;
    auto appearance = source.clipboardAppearance;
    auto pixels = std::make_shared<PinnedPixels>(std::move(source));
    auto result = fromProducer(
        [pixels = std::move(pixels)](const ScreenshotExportCancellation& cancellation) {
            QMutexLocker lock(&pixels->mutex);
            if (cancellation.isCancellationRequested())
                return QImage{};
            if (!pixels->image.isNull())
                return pixels->image;
            QImage image = renderScreenshotPinnedViewport(
                *pixels->source, [&] { return cancellation.isCancellationRequested(); });
            if (!image.isNull()) {
                pixels->image = image;
                // Copies of this source share the immutable rendered pixels. Once
                // ready, retain no background, document, or reconstruction donors.
                pixels->source.reset();
            }
            return image;
        });
    result.m_clipboardPlacement = std::move(placement);
    result.m_clipboardAppearance = std::move(appearance);
    return result;
}

ScreenshotExportSource
ScreenshotExportSource::fromRecognitionImage(ScreenshotRecognitionImageSnapshot snapshot) {
    return fromProducer(
        [snapshot = std::move(snapshot)](const ScreenshotExportCancellation& cancellation) {
            return renderScreenshotRecognitionImage(
                snapshot, [&]() { return cancellation.isCancellationRequested(); });
        });
}
