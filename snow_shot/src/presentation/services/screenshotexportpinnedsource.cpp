#include "snow_shot/presentation/screenshotexportartifact.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>

#include <memory>
#include <utility>

namespace {
SnowCanvasRuntime* workerRuntime() {
    thread_local std::unique_ptr<SnowCanvasRuntime> runtime;
    if (runtime == nullptr) {
        runtime = std::make_unique<SnowCanvasRuntime>(
            SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
    }
    return runtime->isValid() ? runtime.get() : nullptr;
}

QImage renderPinnedViewport(const ScreenshotPinnedViewportExportSource& source) {
    SnowCanvasRuntime* runtime = workerRuntime();
    if (source.backgroundImage.isNull() || !source.backgroundCanvasRect.isValid() ||
        source.backgroundCanvasRect.isEmpty() || !source.contentPixelSize.isValid() ||
        source.contentPixelSize.isEmpty() || runtime == nullptr) {
        return {};
    }
    if (!source.documentSession.isEmpty() &&
        !runtime->restoreDocumentSession(source.documentSession)) {
        return {};
    }
    if (source.documentSession.isEmpty() && !runtime->clearDocumentPreservingViewports()) {
        return {};
    }
    const QList<CanvasExportSource> sources{
        CanvasExportSource{source.backgroundImage, source.backgroundCanvasRect}};
    QImage content =
        runtime->renderToImage(source.backgroundCanvasRect, source.contentPixelSize, sources);
    return content.isNull() ? QImage{}
                            : ScreenshotResultCompositor::compose(content, source.resultStyle);
}
} // namespace

ScreenshotExportSource
ScreenshotExportSource::fromPinnedViewport(ScreenshotPinnedViewportExportSource source) {
    return fromProducer(
        [source = std::move(source)](const ScreenshotExportCancellation& cancellation) {
            return cancellation.isCancellationRequested() ? QImage{} : renderPinnedViewport(source);
        });
}
