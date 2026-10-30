#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/presentation/screenshotselectioncomposition.h"
#include "snow_shot/image/screenshotlayoutcompaction.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

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

QImage renderPinnedViewport(const ScreenshotPinnedViewportExportSource& source,
                            const ScreenshotExportCancellation& cancellation) {
    const auto releaseCaches = qScopeGuard([&] {
        ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
        resetScreenshotLayoutCompactionCache();
        if (source.resultStyle.region)
            source.resultStyle.region->clearDerivedCache();
    });
    if (source.backgroundImage.isNull() || !source.backgroundCanvasRect.isValid() ||
        source.backgroundCanvasRect.isEmpty() || !source.contentPixelSize.isValid() ||
        source.contentPixelSize.isEmpty()) {
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
            const QImage image = renderPinnedViewport(full, cancellation);
            const auto layout = ScreenshotResultCompositor::layoutForContent(
                source.contentPixelSize, source.resultStyle);
            const QPoint offset(qRound((visible.x() - source.backgroundCanvasRect.x()) * sx) -
                                    layout.outputRect.x(),
                                qRound((visible.y() - source.backgroundCanvasRect.y()) * sy) -
                                    layout.outputRect.y());
            return image.isNull() ? QImage() : image.copy(QRect(offset, output));
        }
        QList<SnowCanvasBaseImageSource> sampling = source.samplingSources;
        if (sampling.isEmpty())
            sampling.append({source.backgroundImage, source.backgroundCanvasRect});
        QList<CanvasExportSource> sources;
        for (const auto& item : sampling)
            sources.append({item.image, item.canvasRect, item.coverage});
        QImage result;
        if (source.documentSession.isEmpty()) {
            result = SnowCanvasRuntime::renderImageSourcesToImage(visible, output, sources);
        } else {
            SnowCanvasRuntime runtime(
                SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
            if (!runtime.isValid() || !runtime.restoreDocumentSession(source.documentSession) ||
                !runtime.prepareSmartEraseForExport(source.smartErase, sampling, visible, [&] {
                    return cancellation.isCancellationRequested();
                }))
                return {};
            result = runtime.renderToImage(visible, output, sources);
        }
        if (result.isNull() || cancellation.isCancellationRequested())
            return {};
        result.setColorSpace(source.backgroundImage.colorSpace());
        return ScreenshotResultCompositor::compose(result, {}, 1.0, source.outputOpacity);
    }
    QImage content;
    if (source.documentSession.isEmpty()) {
        // An empty document needs only the source projection. Match the canvas
        // export surface and painter hints without constructing a runtime.
        content =
            snowCanvasAllocateImage(source.contentPixelSize, QImage::Format_ARGB32_Premultiplied);
        if (content.isNull())
            return {};
        content.fill(Qt::transparent);
        QPainter painter(&content);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(QPointF(), QSizeF(source.contentPixelSize)),
                          source.backgroundImage);
    } else {
        // Imported document and reconstruction pixels belong to this export.
        SnowCanvasRuntime runtime(
            SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
        if (!runtime.isValid() || !runtime.restoreDocumentSession(source.documentSession) ||
            cancellation.isCancellationRequested())
            return {};
        QList<SnowCanvasBaseImageSource> samplingSources = source.samplingSources;
        if (samplingSources.isEmpty())
            samplingSources.append({source.backgroundImage, source.backgroundCanvasRect});
        if (!runtime.prepareSmartEraseForExport(
                source.smartErase, samplingSources, source.backgroundCanvasRect,
                [&cancellation] { return cancellation.isCancellationRequested(); }))
            return {};
        QList<CanvasExportSource> sources;
        sources.reserve(samplingSources.size());
        for (const auto& sampling : samplingSources)
            sources.append({sampling.image, sampling.canvasRect, sampling.coverage});
        QImage background = source.backgroundImage;
        if (background.size() != source.contentPixelSize) {
            background = snowCanvasAllocateImage(source.contentPixelSize,
                                                 QImage::Format_ARGB32_Premultiplied);
            if (background.isNull() || cancellation.isCancellationRequested())
                return {};
            background.fill(Qt::transparent);
            QPainter painter(&background);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRectF(QPointF(), QSizeF(source.contentPixelSize)),
                              source.backgroundImage);
        }
        if (cancellation.isCancellationRequested())
            return {};
        if (source.resultStyle.region) {
            auto result = ScreenshotResultCompositor::compose(background, source.resultStyle);
            const auto layout = ScreenshotResultCompositor::layoutForContent(
                source.contentPixelSize, source.resultStyle);
            result = renderScreenshotSelectionAnnotations(runtime, source.backgroundCanvasRect,
                                                          source.contentPixelSize, sources,
                                                          std::move(result), layout);
            return cancellation.isCancellationRequested()
                       ? QImage{}
                       : ScreenshotResultCompositor::compose(result, {}, 1.0, source.outputOpacity);
        }
        content = runtime.renderToImage(source.backgroundCanvasRect, source.contentPixelSize,
                                        sources, background);
    }
    if (cancellation.isCancellationRequested())
        return {};
    content.setColorSpace(source.backgroundImage.colorSpace());
    return content.isNull() ? QImage{}
                            : ScreenshotResultCompositor::compose(content, source.resultStyle, 1.0,
                                                                  source.outputOpacity);
}
} // namespace

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
            QImage image = renderPinnedViewport(*pixels->source, cancellation);
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
