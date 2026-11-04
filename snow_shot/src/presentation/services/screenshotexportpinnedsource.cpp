#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "screenshotpinnedrenderbytes_p.h"

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
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct PinnedPixels final {
    explicit PinnedPixels(ScreenshotPinnedViewportExportSource value) : source(std::move(value)) {}

    QMutex mutex;
    std::optional<ScreenshotPinnedViewportExportSource> source;
    QImage image;
};

std::optional<int> roundedPixel(qreal value) {
    const auto rounded = std::round(value);
    if (!std::isfinite(rounded) || rounded < std::numeric_limits<int>::min() ||
        rounded > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(rounded);
}

bool finiteRect(const QRectF& rect) {
    return rect.isValid() && !rect.isEmpty() && std::isfinite(rect.left()) &&
           std::isfinite(rect.top()) && std::isfinite(rect.right()) && std::isfinite(rect.bottom());
}

bool sharesNativePinnedBackground(const ScreenshotPinnedViewportExportSource& source,
                                  const QRectF& outputRect, const QSize& outputSize) {
    return source.documentSession.isEmpty() && source.textAnnotations.empty() &&
           !source.recognitionOverlay && source.presentationImage.isNull() &&
           outputRect == source.backgroundCanvasRect &&
           outputSize == source.backgroundImage.size() && outputRect.size() == QSizeF(outputSize) &&
           source.backgroundImage.devicePixelRatio() == 1 &&
           source.backgroundImage.format() == QImage::Format_ARGB32_Premultiplied;
}

std::optional<qint64> renderingBytes(const ScreenshotPinnedViewportExportSource& source,
                                     const ScreenshotPinnedViewportRenderPlan& plan) {
    using namespace screenshot_pinned_render;
    const auto style = ScreenshotResultCompositor::normalizedStyle(source.resultStyle);
    const QRect compositionRect = plan.composeVisibleOnly
                                      ? ScreenshotResultCompositor::viewportRenderRect(
                                            source.contentPixelSize, style, plan.outputPixelRect)
                                      : plan.resultLayout.outputRect;
    const auto content =
        rasterBytes(plan.contentPixelRect ? plan.contentPixelRect->size() : plan.renderPixelSize);
    const auto fullComposed = rasterBytes(plan.resultLayout.outputRect.size());
    const auto composed = rasterBytes(compositionRect.size());
    const auto output = rasterBytes(plan.outputSize());
    const auto mask =
        rasterBytes(plan.composeVisibleOnly && !style.region ? compositionRect.size()
                                                             : plan.resultLayout.outputRect.size(),
                    8);
    if (!content || !fullComposed || !composed || !output || !mask)
        return std::nullopt;
    const auto colorSpace = source.backgroundImage.colorSpace();
    const bool sharesBackground =
        !plan.contentPixelRect &&
        sharesNativePinnedBackground(source, plan.renderCanvasRect, plan.renderPixelSize);
    const bool convertsColor = colorSpace.isValid() && colorSpace != QColorSpace(QColorSpace::SRgb);
    const bool sharesPixels = sharesBackground && !convertsColor;
    qint64 bytes = sharesPixels ? 4096 : *content;
    if (convertsColor && !sharesBackground && !addBytes(bytes, *content))
        return std::nullopt;
    if (plan.contentPixelRect &&
        !addBytes(bytes,
                  std::max(qint64(8) * 1024 * 1024, qint64(plan.renderPixelSize.width()) * 4)))
        return std::nullopt;
    const bool document = !source.documentSession.isEmpty();
    // Drawing can coexist with a prepared backdrop and canvas compositor scratch.
    if (document)
        for (int index = 0; index < 3; ++index)
            if (!addBytes(bytes, *content))
                return std::nullopt;
    const bool composition = style.region || style.cornerRadius > 0 || style.effectWidth() > 0;
    if (composition) {
        if (!addBytes(bytes, *composed))
            return std::nullopt;
        if (style.region && style.borderType == ScreenshotSelectionBorderType::Shadow) {
            if (style.effectWidth() > 0) {
                // Region shadows use bounded cores with a finite blur halo. Sparse small
                // regions may use one untiled raster of at most one million pixels.
                const qint64 effect = plan.resultLayout.effectInsets.left();
                const qint64 side = std::max(qint64(512), effect * 8) + effect * 2 + 4;
                const auto size = plan.resultLayout.outputRect.size();
                const qint64 pixels = std::min(
                    *fullComposed / 4,
                    std::max(qint64(1024) * 1024, std::min(side, qint64(size.width())) *
                                                      std::min(side, qint64(size.height()))));
                for (int index = 0; index < 3; ++index)
                    if (!addBytes(bytes, pixels * 4))
                        return std::nullopt;
                if (!addBytes(bytes, qint64(size.width()) * 4))
                    return std::nullopt;
            } else if (!addBytes(bytes, *composed)) {
                return std::nullopt;
            }
            if (!addBytes(bytes, *mask))
                return std::nullopt;
            // Export masks/shadows can stay cached while annotations are rendered.
            if (document && !addBytes(bytes, std::min(qint64(16) * 1024 * 1024, *composed)))
                return std::nullopt;
        } else {
            qint64 scratch = style.region || style.cornerRadius > 0 ? *mask : 0;
            if (plan.composeVisibleOnly && style.region && style.effectWidth() > 0 &&
                (!addBytes(scratch, *mask) || !addBytes(scratch, *output)))
                return std::nullopt;
            if (style.borderType == ScreenshotSelectionBorderType::Shadow &&
                style.effectWidth() > 0) {
                const qint64 radius = qRound(std::min(
                    qreal(style.cornerRadius),
                    std::min(plan.renderPixelSize.width(), plan.renderPixelSize.height()) / 2.0));
                const qint64 side = 2 * (radius + style.effectWidth()) + 1;
                if (side > std::numeric_limits<int>::max())
                    return std::nullopt;
                const auto asset =
                    rasterBytes(QSize(static_cast<int>(side), static_cast<int>(side)));
                if (!asset)
                    return std::nullopt;
                scratch = std::max(scratch, *asset);
            }
            if (!addBytes(bytes, scratch))
                return std::nullopt;
        }
        if (document && style.region && source.outputOpacity < 1 && !addBytes(bytes, *composed))
            return std::nullopt;
    } else if (source.outputOpacity < 1 && !addBytes(bytes, *content)) {
        return std::nullopt;
    }
    if (plan.cropAfterRender &&
        (!plan.composeVisibleOnly || compositionRect != plan.outputPixelRect) &&
        !addBytes(bytes, *output))
        return std::nullopt;
    return bytes;
}

QImage renderPinnedBackground(const ScreenshotPinnedViewportExportSource& source,
                              const QRectF& outputRect, const QSize& outputSize,
                              const std::function<bool()>& cancelled) {
    // An unchanged native backdrop can be read directly by the canvas exporter.
    if (!source.documentSession.isEmpty() && source.textAnnotations.empty() &&
        !source.recognitionOverlay && source.presentationImage.isNull() &&
        outputRect == source.backgroundCanvasRect && outputSize == source.backgroundImage.size())
        return source.backgroundImage;
    if (sharesNativePinnedBackground(source, outputRect, outputSize)) {
        // Match a freshly projected raster's metadata while retaining immutable native pixels.
        // The owner also makes mutations of either the input or the result detach safely.
        auto owner = std::make_unique<QImage>(source.backgroundImage);
        QImage background(
            owner->constBits(), owner->width(), owner->height(), owner->bytesPerLine(),
            owner->format(), [](void* context) { delete static_cast<QImage*>(context); },
            owner.get());
        if (!background.isNull()) {
            owner.release();
            background.setColorSpace(source.backgroundImage.colorSpace());
        }
        return background;
    }
    const QImage& backdrop =
        source.presentationImage.isNull() ? source.backgroundImage : source.presentationImage;
    const QRectF backdropRect = source.presentationImage.isNull() ? source.backgroundCanvasRect
                                                                  : source.presentationCanvasRect;
    QImage background = SnowCanvasRuntime::renderImageSourcesToImage(outputRect, outputSize,
                                                                     {{backdrop, backdropRect}});
    if (background.isNull())
        return background;
    background.setColorSpace(source.backgroundImage.colorSpace());
    if (source.textAnnotations.empty() && !source.recognitionOverlay)
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
    if (source.recognitionOverlay && !paintScreenshotRecognitionOverlay(
                                         painter, *source.recognitionOverlay, toPixels, cancelled))
        return {};
    if (!paintScreenshotTextAnnotations(painter, source.textAnnotations, toPixels, nullptr,
                                        cancelled))
        return {};
    return background;
}
} // namespace

std::optional<ScreenshotPinnedViewportRenderPlan>
screenshotPinnedViewportRenderPlan(const ScreenshotPinnedViewportExportSource& source) {
    if (source.backgroundImage.isNull() || !finiteRect(source.backgroundCanvasRect) ||
        source.contentPixelSize.isEmpty())
        return std::nullopt;
    ScreenshotPinnedViewportRenderPlan plan;
    plan.renderCanvasRect = source.backgroundCanvasRect;
    plan.renderPixelSize = source.contentPixelSize;
    plan.resultLayout =
        ScreenshotResultCompositor::layoutForContent(source.contentPixelSize, source.resultStyle);
    if (!plan.resultLayout.isValid())
        return std::nullopt;
    plan.outputPixelRect = plan.resultLayout.outputRect;
    if (source.visibleCanvasRect) {
        const QRectF visible = *source.visibleCanvasRect;
        if (!finiteRect(visible))
            return std::nullopt;
        const qreal sx = source.contentPixelSize.width() / source.backgroundCanvasRect.width();
        const qreal sy = source.contentPixelSize.height() / source.backgroundCanvasRect.height();
        const auto width = roundedPixel(visible.width() * sx);
        const auto height = roundedPixel(visible.height() * sy);
        if (!width || !height)
            return std::nullopt;
        const QSize output(std::max(1, *width), std::max(1, *height));
        plan.cropAfterRender = source.resultStyle.region || source.resultStyle.cornerRadius > 0 ||
                               source.resultStyle.effectWidth() > 0;
        if (plan.cropAfterRender) {
            const auto x = roundedPixel((visible.x() - source.backgroundCanvasRect.x()) * sx -
                                        plan.resultLayout.outputRect.x());
            const auto y = roundedPixel((visible.y() - source.backgroundCanvasRect.y()) * sy -
                                        plan.resultLayout.outputRect.y());
            if (!x || !y || qint64(*x) + output.width() - 1 > std::numeric_limits<int>::max() ||
                qint64(*y) + output.height() - 1 > std::numeric_limits<int>::max())
                return std::nullopt;
            plan.outputPixelRect = QRect(QPoint(*x, *y), output);
            // Region annotations render over the composed capture and may sample its full
            // backdrop. Keep that ordering until the renderer can request composed halos.
            plan.composeVisibleOnly =
                source.documentSession.isEmpty() || !source.resultStyle.region;
        } else {
            plan.renderCanvasRect = visible;
            plan.renderPixelSize = output;
            plan.resultLayout = ScreenshotResultCompositor::layoutForContent(output, {});
            plan.outputPixelRect = plan.resultLayout.outputRect;
        }
    }
    if (plan.composeVisibleOnly && source.documentSession.isEmpty() &&
        source.textAnnotations.empty() && !source.recognitionOverlay &&
        source.presentationImage.isNull() &&
        !sharesNativePinnedBackground(source, plan.renderCanvasRect, plan.renderPixelSize)) {
        auto contentRect = ScreenshotResultCompositor::viewportRenderRect(
                               source.contentPixelSize, source.resultStyle, plan.outputPixelRect)
                               .intersected(plan.resultLayout.contentRect)
                               .translated(-plan.resultLayout.contentRect.topLeft());
        // A transparent placeholder supplies metadata for a shadow-only viewport.
        if (contentRect.isEmpty())
            contentRect = QRect(0, 0, 1, 1);
        plan.contentPixelRect = contentRect;
    }
    const auto bytes = renderingBytes(source, plan);
    if (!bytes)
        return std::nullopt;
    plan.workingBytes = *bytes;
    return plan;
}

namespace {
QImage renderPinnedViewport(const ScreenshotPinnedViewportExportSource& source,
                            const ScreenshotPinnedViewportRenderPlan& plan,
                            const std::function<bool()>& cancelled) {
    const auto stopped = [&] { return cancelled && cancelled(); };
    const auto& outputRect = plan.renderCanvasRect;
    const auto& outputSize = plan.renderPixelSize;
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
                                 source.resultStyle.region.has_value() ||
                                 source.recognitionOverlay.has_value() ||
                                 !source.presentationImage.isNull();
    QImage background;
    if (needsBackground) {
        if (plan.contentPixelRect) {
            background = SnowCanvasRuntime::renderImageSourcesToImage(
                outputRect, outputSize, {{source.backgroundImage, source.backgroundCanvasRect}},
                *plan.contentPixelRect);
            background.setColorSpace(source.backgroundImage.colorSpace());
        } else {
            background = renderPinnedBackground(source, outputRect, outputSize, cancelled);
        }
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
            result = renderScreenshotSelectionAnnotations(runtime, outputRect, outputSize, sources,
                                                          std::move(result), plan.resultLayout);
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
    if (plan.composeVisibleOnly)
        return ScreenshotResultCompositor::composeViewport(
            content, source.contentPixelSize, plan.contentPixelRect.value_or(QRect()).topLeft(),
            source.resultStyle, plan.outputPixelRect, source.outputOpacity);
    return content.isNull()
               ? QImage{}
               : ScreenshotResultCompositor::compose(
                     content,
                     source.visibleCanvasRect && !plan.cropAfterRender ? ScreenshotResultStyle{}
                                                                       : source.resultStyle,
                     1.0, source.outputOpacity);
}
} // namespace

QImage renderScreenshotPinnedViewport(const ScreenshotPinnedViewportExportSource& source,
                                      const std::function<bool()>& cancelled) {
    const auto releaseCaches = qScopeGuard([&] {
        ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
        resetScreenshotLayoutCompactionCache();
        if (source.resultStyle.region)
            source.resultStyle.region->clearDerivedCache();
    });
    if (cancelled && cancelled())
        return {};
    const auto plan = screenshotPinnedViewportRenderPlan(source);
    if (!plan)
        return {};
    QImage image = renderPinnedViewport(source, *plan, cancelled);
    if (image.isNull() || (cancelled && cancelled()))
        return {};
    return plan->cropAfterRender && !plan->composeVisibleOnly
               ? snowCanvasCopyImage(image, plan->outputPixelRect)
               : image;
}

ScreenshotExportSource
ScreenshotExportSource::fromPinnedViewport(ScreenshotPinnedViewportExportSource source) {
    const auto plan = screenshotPinnedViewportRenderPlan(source);
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
    result.m_renderingBytes = plan ? plan->workingBytes : std::numeric_limits<qint64>::max();
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
