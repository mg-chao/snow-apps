// SPDX-License-Identifier: GPL-3.0-or-later
#include "../pinned/screenshotpinnedcropgeometry.h"
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <algorithm>

namespace snow_shot::presentation {
namespace {
QString loadError() {
    return QCoreApplication::translate("ImageEditingWindow", "The image could not be loaded.");
}

bool hasEditableSelection(const storage::PersistedSelection& selection) {
    // The editor owns one rectangular base image and annotations in the original canvas space.
    // Masks and effects require their own clipping/layout state; restoring only their raw donors
    // would reveal excluded pixels or change annotation placement. Their stored composite is the
    // faithful representation until that state can also be edited. Background and compaction
    // preferences have no visual effect on a plain rectangle without an effect inset.
    return !selection.region && selection.cornerRadius == 0 && selection.effectWidth() == 0;
}
} // namespace

QImage renderImageEditingBaseImage(const ScreenshotImageSource& source, const QRectF& canvasRect,
                                   const QSize& pixelSize) {
    if (!source.isValid())
        return {};
    QList<CanvasExportSource> sources;
    if (source.isMaterialized()) {
        sources.append({source.materializedImage, source.materializedCanvasRect, {}});
    } else {
        sources.reserve(source.layers.size());
        for (const auto& layer : source.layers) {
            const auto visible = layer.imageCanvasRect.intersected(canvasRect);
            if (visible.isEmpty())
                continue;
            // The output already clips the selection. Avoid an additional rounded clip at
            // fractional source edges unless the layer excludes pixels inside the output.
            const auto coverage = layer.destinationCanvasRect.contains(visible)
                                      ? QRectF{}
                                      : layer.destinationCanvasRect;
            sources.append({layer.image, layer.imageCanvasRect, coverage});
        }
    }
    auto image = SnowCanvasRuntime::renderImageSourcesToImage(canvasRect, pixelSize, sources);
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

void restrictImageEditingSource(ImageEditingSource& source, const QRectF& visible) {
    const QRectF full = source.canvasRect;
    if (source.image.isNull())
        source.image = renderImageEditingBaseImage(source.layeredImage, full, source.pixelSize);
    if (source.image.isNull() || full.isEmpty() || visible.isEmpty())
        return;
    const auto colorSpace = source.image.colorSpace();
    const qreal sx = source.image.width() / full.width();
    const qreal sy = source.image.height() / full.height();
    source.samplingSources.append({source.image, full});
    QRectF rasterBounds = full;
    if (source.resultStyle) {
        const auto style = ScreenshotResultCompositor::scaledStyle(*source.resultStyle, sx);
        const auto layout =
            ScreenshotResultCompositor::layoutForContent(source.image.size(), style);
        source.image = ScreenshotResultCompositor::compose(source.image, style);
        rasterBounds =
            QRectF(full.x() + layout.outputRect.x() / sx, full.y() + layout.outputRect.y() / sy,
                   layout.outputRect.width() / sx, layout.outputRect.height() / sy);
        source.resultStyle.reset();
    }
    source.image = SnowCanvasRuntime::renderImageSourcesToImage(
        visible,
        QSize(std::max(1, qRound(visible.width() * sx)),
              std::max(1, qRound(visible.height() * sy))),
        {{source.image, rasterBounds}});
    source.image.setColorSpace(colorSpace);
    source.canvasRect = visible;
    source.pixelSize = source.image.size();
    source.layeredImage = {};
}

ImageEditingSource readImageEditingHistory(storage::CaptureHistoryRepository& repository,
                                           const storage::CaptureHistoryRecord& record) {
    ImageEditingSource result;
    const auto payload = hasEditableSelection(record.selection)
                             ? repository.loadPreservingRecord(record)
                             : std::nullopt;
    const bool mapped =
        record.displays.size() == 1 ||
        std::all_of(record.displays.cbegin(), record.displays.cend(), [](const auto& display) {
            return display.sourceCanvasRect.has_value() || display.sourceCanvasOrigin.has_value();
        });
    if (payload && mapped && payload->displayImages.size() == record.displays.size()) {
        SnowCanvasRuntime validator;
        if (validator.restoreDocumentHistory(payload->canvasHistory) ||
            validator.restoreDocumentSession(payload->canvasHistory)) {
            const QRectF bounds(record.selection.rectangle);
            QList<ScreenshotImageLayer> layers;
            qreal scale = 1;
            for (qsizetype i = 0; i < record.displays.size(); ++i) {
                const auto& display = record.displays[i];
                const QImage& image = payload->displayImages[i];
                QRectF frame = display.sourceCanvasRect.value_or(
                    QRect(display.sourceCanvasOrigin.value_or(record.canvasBounds.topLeft()),
                          image.size()));
                if (frame.isEmpty())
                    continue;
                result.samplingSources.append({image, frame, {}});
                const QRectF visible = frame.intersected(bounds);
                // Match ordinary screenshot export: only intersecting point-space sources
                // contribute their recorded backing scale, including source-size rounding.
                if (!visible.isEmpty() && display.canvasUsesPoints)
                    scale = std::max(scale, display.backingScale);
                if (!visible.isEmpty())
                    layers.append({image, frame, visible});
                if (record.cursorVisible && display.cursor && i < payload->cursorPatches.size() &&
                    !payload->cursorPatches[i].isNull()) {
                    const auto pixelRect = display.cursor->pixelRect;
                    const qreal sx = frame.width() / image.width();
                    const qreal sy = frame.height() / image.height();
                    const QRectF cursorRect(frame.x() + pixelRect.x() * sx,
                                            frame.y() + pixelRect.y() * sy, pixelRect.width() * sx,
                                            pixelRect.height() * sy);
                    const auto cursorVisible = cursorRect.intersected(bounds);
                    result.samplingSources.append(
                        {payload->cursorPatches[i], cursorRect, {}, false});
                    if (!cursorVisible.isEmpty())
                        layers.append(
                            {payload->cursorPatches[i], cursorRect, cursorVisible, false});
                }
            }
            result.image = renderImageEditingBaseImage(
                ScreenshotImageSource::fromLayers(layers), bounds,
                screenshotSelectionRenderedPixelSize(record.selection.rectangle.size(), scale));
            if (!result.image.isNull()) {
                result.canvasRect = bounds;
                result.canvasHistory = validator.serializeDocumentHistory();
                return result;
            }
        }
    }
    const auto composite = repository.loadResultImagePreservingRecord(record);
    if (composite) {
        result.samplingSources.clear();
        result.image = *composite;
        result.canvasRect = QRectF(QPointF(), QSizeF(result.image.size()));
        result.flattened = record.canvasBytes > 0;
    } else
        result.error = loadError();
    return result;
}
ImageEditingSource readImageEditingPinned(storage::PinnedWindowRecord record,
                                          const ScreenshotPinnedTextPreparation& preparation) {
    ImageEditingSource result;
    const auto style = decodeScreenshotResultStyle(record.resultStyle);
    if (!style) {
        result.error = loadError();
        return result;
    }
    if (style->region || style->cornerRadius != 0 || style->effectWidth() != 0)
        result.resultStyle = *style;
    if (record.sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
        ScreenshotClipboardOriginalContent original;
        original.html = record.originalHtml;
        original.text = record.originalText;
        original.textStyle = record.textStyle;
        original.textFormat = record.textFormat;
        const auto rendered =
            prepareScreenshotPinnedText(original, record.firstCreationTextDpi, preparation);
        if (!rendered) {
            result.error = loadError();
            return result;
        }
        record.image = rendered->image;
    } else if (record.animationSource)
        record.image = record.animationSource->firstFrame;
    result.image =
        snowCanvasTransformImage(record.image, record.imageTransform, Qt::SmoothTransformation);
    result.canvasRect = record.contentCanvasRect;
    if (!result.canvasRect.isValid())
        result.canvasRect = QRectF(QPointF(), QSizeF(result.image.size()));
    result.canvasSession = record.canvasSession;
    result.recognition =
        ScreenshotPinnedWindow::decodeRecognitionSnapshot(record.recognitionResults);
    result.originalFileName = record.originalFileName;
    if (record.cropRect && !result.image.isNull() &&
        screenshot_pinned_crop_geometry::valid(*record.cropRect) &&
        QRectF(record.image.rect()).contains(*record.cropRect)) {
        const auto full = result.canvasRect;
        const auto visible = screenshot_pinned_crop_geometry::sourceToCanvas(
                                 record.image.size(), record.imageTransform, full)
                                 .mapRect(*record.cropRect);
        restrictImageEditingSource(result, visible);
    }
    if (result.image.isNull())
        result.error = loadError();
    return result;
}

ScreenshotExportSource imageEditingCompositeSource(const ImageEditingSource& source) {
    if (!source.resultStyle)
        return {};
    return imageEditingExportSource(source);
}

ScreenshotExportSource imageEditingExportSource(const ImageEditingSource& source) {
    auto outputStyle = source.resultStyle.value_or(ScreenshotResultStyle{});
    const qreal scale =
        source.canvasRect.width() > 0 ? source.image.width() / source.canvasRect.width() : 1.0;
    outputStyle = ScreenshotResultCompositor::scaledStyle(outputStyle, scale);
    ScreenshotPinnedViewportExportSource viewport{source.canvasSession, source.image,
                                                  source.canvasRect,    source.image.size(),
                                                  outputStyle,          source.smartErase};
    viewport.samplingSources = source.samplingSources;
    return ScreenshotExportSource::fromPinnedViewport(std::move(viewport));
}

ImageEditingSource completeImageEditingComposite(ImageEditingSource source, QImage image) {
    source.image = std::move(image);
    source.layeredImage = {};
    source.pixelSize = source.image.size();
    source.canvasRect = QRectF(QPointF(), QSizeF(source.pixelSize));
    source.canvasSession.clear();
    source.canvasHistory.clear();
    source.smartErase = {};
    source.samplingSources.clear();
    source.recognition = {};
    source.resultStyle.reset();
    source.flattened = true;
    if (source.image.isNull())
        source.error = loadError();
    return source;
}
} // namespace snow_shot::presentation
