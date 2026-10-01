#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QColorSpace>

#include <utility>

namespace {
QImage renderPinnedViewport(const ScreenshotPinnedViewportExportSource& source) {
    if (source.backgroundImage.isNull() || !source.backgroundCanvasRect.isValid() ||
        source.backgroundCanvasRect.isEmpty() || !source.contentPixelSize.isValid() ||
        source.contentPixelSize.isEmpty()) {
        return {};
    }
    // Imported document and reconstruction pixels belong to this export, not
    // the worker thread, which can remain idle for the application's lifetime.
    SnowCanvasRuntime runtime(
        SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
    if (!runtime.isValid() || (!source.documentSession.isEmpty() &&
                               !runtime.restoreDocumentSession(source.documentSession))) {
        return {};
    }
    runtime.restoreSmartEraseSnapshot(source.smartErase);
    const QList<CanvasExportSource> sources{
        CanvasExportSource{source.backgroundImage, source.backgroundCanvasRect}};
    QImage content =
        runtime.renderToImage(source.backgroundCanvasRect, source.contentPixelSize, sources);
    content.setColorSpace(source.backgroundImage.colorSpace());
    ScreenshotResultCompositor::restoreBakedExterior(content, source.backgroundImage,
                                                     source.bakedSelectionPath);
    return content.isNull() ? QImage{}
                            : ScreenshotResultCompositor::compose(content, source.resultStyle, 1.0,
                                                                  source.outputOpacity);
}
} // namespace

ScreenshotExportSource
ScreenshotExportSource::fromPinnedViewport(ScreenshotPinnedViewportExportSource source) {
    auto placement = source.clipboardPlacement;
    auto appearance = source.clipboardAppearance;
    auto result = fromProducer(
        [source = std::move(source)](const ScreenshotExportCancellation& cancellation) {
            return cancellation.isCancellationRequested() ? QImage{} : renderPinnedViewport(source);
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
