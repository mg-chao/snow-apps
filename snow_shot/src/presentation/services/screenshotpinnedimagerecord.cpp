// SPDX-License-Identifier: GPL-3.0-or-later
#include "../pinned/screenshotpinnedcropgeometry.h"
#include "screenshotpinnedrenderbytes_p.h"
#include "snow_shot/presentation/screenshotpinnedrenderpreparation.h"
#include "snow_shot/presentation/screenshotpinnedrecognition.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"
#include "snow_shot/presentation/screenshotocrlayout.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QCoreApplication>
#include <QTextDocument>
#include <limits>
#include <utility>

namespace snow_shot::presentation {
namespace {
QString loadError() {
    return QCoreApplication::translate("ImageEditingWindow", "The image could not be loaded");
}

struct PreparedPinnedImage final {
    ScreenshotPinnedViewportExportSource viewport;
    ScreenshotPinnedImageSnapshot snapshot;
};

PreparedPinnedImage
preparePinnedImageRecord(storage::PinnedWindowRecord record,
                         const ScreenshotPinnedRenderPreparation& preparation,
                         const ScreenshotClipboardContentReader::CancellationCheck& cancelled,
                         const ScreenshotClipboardContentReader::AllocationCheck& allocate) {
    PreparedPinnedImage prepared;
    auto& result = prepared.viewport;
    auto& snapshot = prepared.snapshot;
    snapshot.label = record.id;
    snapshot.originalFileName = record.originalFileName;
    snapshot.createdUtc = record.createdUtc;
    snapshot.error = loadError();
    const auto stopped = [&] { return cancelled && cancelled(); };
    if (stopped())
        return prepared;
    std::shared_ptr<QTextDocument> formattedDocument;
    qreal formattedDpi = 1;
    const auto style = decodeScreenshotResultStyle(record.resultStyle);
    if (!style)
        return prepared;
    if (record.sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
        ScreenshotClipboardOriginalContent original;
        original.html = record.originalHtml;
        original.text = record.originalText;
        original.textStyle = record.textStyle;
        original.textFormat = record.textFormat;
        const auto rendered = ScreenshotClipboardContentReader::renderOriginalText(
            original, record.firstCreationTextDpi, preparation.text.baseColor, allocate,
            preparation.text.colorSwatchStyle);
        if (!rendered)
            return prepared;
        record.image = rendered->image;
        formattedDocument = rendered->formattedDocument;
        formattedDpi = rendered->formattedTextDevicePixelRatio;
    } else if (record.animationSource)
        record.image = record.animationSource->firstFrame;
    if (record.image.isNull() || stopped())
        return prepared;
    const auto oriented =
        QImage::trueMatrix(record.imageTransform, record.image.width(), record.image.height())
            .mapRect(QRect(QPoint(), record.image.size()))
            .size();
    const qint64 sourceBytes = record.image.sizeInBytes();
    const auto orientedBytes = screenshot_pinned_render::rasterBytes(oriented);
    const auto transformedBytes =
        screenshot_pinned_render::rasterBytes(oriented, std::max(32, record.image.depth()));
    qint64 transformBytes = sourceBytes;
    if (!orientedBytes || !transformedBytes ||
        !screenshot_pinned_render::addBytes(transformBytes, *orientedBytes) ||
        (!record.imageTransform.isIdentity() &&
         (!screenshot_pinned_render::addBytes(transformBytes, *transformedBytes) ||
          !screenshot_pinned_render::addBytes(transformBytes, *transformedBytes))) ||
        (allocate && !allocate(transformBytes)))
        return prepared;
    result.backgroundImage = ScreenshotResultCompositor::normalizeImage(
        snowCanvasTransformImage(record.image, record.imageTransform, Qt::SmoothTransformation));
    if (result.backgroundImage.isNull() || stopped())
        return prepared;
    result.backgroundCanvasRect = record.contentCanvasRect;
    if (!result.backgroundCanvasRect.isValid() || result.backgroundCanvasRect.isEmpty()) {
        const auto source = record.canvasSourceRect;
        result.backgroundCanvasRect =
            source.isValid() && !source.isEmpty()
                ? QRectF(source.topLeft(), QSizeF(result.backgroundImage.width() * source.width() /
                                                      record.image.width(),
                                                  result.backgroundImage.height() *
                                                      source.height() / record.image.height()))
                : QRectF(QPointF(), result.backgroundImage.size());
    }
    auto crop = record.cropRect;
    if (crop && (!screenshot_pinned_crop_geometry::valid(*crop) ||
                 !QRectF(record.image.rect()).contains(*crop)))
        crop.reset();
    if (crop)
        result.visibleCanvasRect =
            screenshot_pinned_crop_geometry::sourceToCanvas(
                record.image.size(), record.imageTransform, result.backgroundCanvasRect)
                .mapRect(*crop);
    const QSizeF baseline = screenshotPinnedScaleReferenceSize(
        record.initialWindowSize, record.exactInitialWindowSize, record.quarterTurns, crop,
        record.image.size(), record.canvasSourceRect, *style);
    const QSize encodedSize = record.thumbnailMode && record.preThumbnailPlacement.isValid()
                                  ? record.preThumbnailPlacement.windowSize
                              : record.thumbnailMode && record.preThumbnailNativeGeometry.isValid()
                                  ? record.preThumbnailNativeGeometry.size()
                              : record.placement.isValid() ? record.placement.windowSize
                                                           : record.nativeGeometry.size();
    const bool hasGeometry = !baseline.isEmpty() && !encodedSize.isEmpty();
    // Standalone callers may supply source-only legacy records without window state.
    const double zoom = hasGeometry
                            ? screenshotPinnedRestoredScalePercent(
                                  baseline, encodedSize, record.scalePercent,
                                  crop.has_value() || !record.exactInitialWindowSize.isEmpty())
                            : 100.0;
    const qreal scale = screenshotPinnedRenderScale(zoom, result.backgroundImage.size(),
                                                    result.backgroundCanvasRect);
    const double width = result.backgroundCanvasRect.width() * scale;
    const double height = result.backgroundCanvasRect.height() * scale;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
        return prepared;
    result.contentPixelSize = {std::max(1, qRound(width)), std::max(1, qRound(height))};
    result.resultStyle = ScreenshotResultCompositor::scaledStyle(*style, scale);
    result.outputOpacity =
        qBound(record.hideToTopMode ? 25 : 1, record.opacityPercent, 100) / 100.0;
    if (!record.animationSource && !record.canvasSession.isEmpty()) {
        SnowCanvasRuntime validator;
        if (!validator.isValid() || !validator.restoreDocumentSession(record.canvasSession))
            return prepared;
        if (validator.hasDocumentContent()) {
            result.documentSession = validator.serializeDocumentSnapshot();
            if (result.documentSession.isEmpty())
                return prepared;
        }
    }
    const auto plan = screenshotPinnedViewportRenderPlan(result);
    if (!plan)
        return prepared;
    snapshot.outputSize = plan->outputSize();
    snapshot.renderingBytes = plan->workingBytes;
    qint64 renderingBytes = sourceBytes;
    if (!screenshot_pinned_render::addBytes(renderingBytes, *orientedBytes) ||
        !screenshot_pinned_render::addBytes(renderingBytes, plan->workingBytes))
        return prepared;
    const bool recognitionVisible =
        record.recognitionVisible && !record.thumbnailMode && !record.hideToTopMode &&
        record.sourceKind != storage::PinnedWindowSourceKind::ClipboardText &&
        !record.animationSource;
    // Filtering owns source-sized copies and bounded blur scratch independently of zoom/crop.
    if (recognitionVisible)
        for (int index = 0; index < 4; ++index)
            if (!screenshot_pinned_render::addBytes(renderingBytes, *orientedBytes))
                return prepared;
    if ((allocate && !allocate(std::max(transformBytes, renderingBytes))) || stopped())
        return prepared;
    auto recognition =
        decodeScreenshotPinnedRecognition(record.recognitionResults, preparation.font);
    ScreenshotTextAnnotationDocument textAnnotations;
    if (!record.textAnnotations.isEmpty() && textAnnotations.restore(record.textAnnotations)) {
        QVector<int> sourceLengths;
        if (textAnnotations.source() == ScreenshotTextAnnotationSource::Formatted &&
            formattedDocument) {
            sourceLengths.append(formattedDocument->characterCount() - 1);
        } else if (textAnnotations.source() == ScreenshotTextAnnotationSource::Ocr &&
                   recognition.text && recognition.text->presentation) {
            for (const auto& line : recognition.text->presentation->lines)
                sourceLengths.append(static_cast<int>(line.text.size()));
        }
        if (!textAnnotations.validForSource(sourceLengths))
            textAnnotations.clear();
        QTransform pixelsToCanvas;
        pixelsToCanvas.translate(result.backgroundCanvasRect.left(),
                                 result.backgroundCanvasRect.top());
        pixelsToCanvas.scale(result.backgroundCanvasRect.width() / result.backgroundImage.width(),
                             result.backgroundCanvasRect.height() /
                                 result.backgroundImage.height());
        const QTransform orientedPixels =
            QImage::trueMatrix(record.imageTransform, record.image.width(), record.image.height()) *
            pixelsToCanvas;
        if (textAnnotations.source() == ScreenshotTextAnnotationSource::Formatted &&
            formattedDocument &&
            textAnnotations.fingerprint() ==
                screenshotFormattedTextAnnotationFingerprint(*formattedDocument)) {
            result.textAnnotations = screenshotFormattedTextAnnotationSnapshot(
                textAnnotations, *formattedDocument,
                QTransform::fromScale(formattedDpi, formattedDpi) * orientedPixels);
        } else if (textAnnotations.source() == ScreenshotTextAnnotationSource::Ocr &&
                   recognition.text && recognition.text->presentation &&
                   textAnnotations.fingerprint() ==
                       screenshotOcrTextAnnotationFingerprint(*recognition.text->presentation) &&
                   !record.canvasSourceRect.isEmpty()) {
            QTransform sourceToPixels;
            sourceToPixels.scale(record.image.width() / record.canvasSourceRect.width(),
                                 record.image.height() / record.canvasSourceRect.height());
            sourceToPixels.translate(-record.canvasSourceRect.left(),
                                     -record.canvasSourceRect.top());
            result.textAnnotations = screenshotOcrTextAnnotationSnapshot(
                textAnnotations, *recognition.text->presentation, sourceToPixels * orientedPixels,
                preparation.font);
        }
    }
    if (recognitionVisible) {
        if (!recognition.text || !recognition.text->error.isEmpty() ||
            !recognition.text->presentation)
            return prepared;
        const bool translated = snow_shot::app::edition::textTranslation &&
                                record.textType == storage::PinnedTextType::TranslationResults;
        auto presentation =
            translated ? recognition.translatedText : recognition.text->presentation;
        if (!presentation || (translated && !ocrTranslationSourceIndices(
                                                *presentation, *recognition.text->presentation)))
            return prepared;
        prepareScreenshotOcrFillColors(*presentation, record.image, record.canvasSourceRect,
                                       preparation.backgroundFill);
        recognition = transformScreenshotPinnedRecognition(
            std::move(recognition), record.canvasSourceRect, record.image.size(),
            record.imageTransform, result.backgroundImage.size(), result.backgroundCanvasRect);
        presentation = translated ? recognition.translatedText : recognition.text->presentation;
        ScreenshotRecognitionImageSnapshot overlay;
        overlay.canvasRect = result.backgroundCanvasRect;
        overlay.lines = presentation->lines;
        overlay.font = preparation.font;
        overlay.textColor = preparation.textColor;
        QRect filteredPixels;
        overlay.filteredImage = renderScreenshotOcrFilteredImage(
            result.backgroundImage, result.backgroundCanvasRect, *presentation,
            preparation.backgroundColor,
            std::max(qreal(1),
                     result.backgroundImage.width() / result.backgroundCanvasRect.width()),
            &filteredPixels);
        if (overlay.filteredImage.isNull())
            return prepared;
        overlay.filteredCanvasRect = screenshotOcrFilteredImageCanvasRect(
            result.backgroundCanvasRect, result.backgroundImage.size(), filteredPixels);
        result.recognitionOverlay = std::move(overlay);
    }
    if (!stopped())
        snapshot.error.clear();
    return prepared;
}
} // namespace

ScreenshotPinnedImageSnapshot
readScreenshotPinnedImageSnapshot(storage::PinnedWindowRecord record,
                                  const ScreenshotPinnedRenderPreparation& preparation,
                                  ScreenshotClipboardContentReader::CancellationCheck cancelled,
                                  ScreenshotClipboardContentReader::AllocationCheck allocate) {
    auto prepared = preparePinnedImageRecord(std::move(record), preparation, cancelled, allocate);
    if (prepared.snapshot.error.isEmpty())
        prepared.snapshot.source =
            ScreenshotExportSource::fromPinnedViewport(std::move(prepared.viewport));
    return std::move(prepared.snapshot);
}

ScreenshotExportImageResult
renderScreenshotPinnedImageRecord(storage::PinnedWindowRecord record,
                                  const ScreenshotPinnedRenderPreparation& preparation,
                                  ScreenshotClipboardContentReader::CancellationCheck cancelled,
                                  ScreenshotClipboardContentReader::AllocationCheck allocate) {
    auto prepared = preparePinnedImageRecord(std::move(record), preparation, cancelled, allocate);
    if (!prepared.snapshot.error.isEmpty())
        return {{}, std::move(prepared.snapshot.error)};
    auto image = renderScreenshotPinnedViewport(prepared.viewport, cancelled);
    return image.isNull() ? ScreenshotExportImageResult{{}, loadError()}
                          : ScreenshotExportImageResult{std::move(image), {}};
}

} // namespace snow_shot::presentation
