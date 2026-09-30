#include "snow_shot/presentation/screenshotexportartifact.h"

#include "snow_shot/presentation/screenshotscrollingsnapshot.h"

#include <utility>

ScreenshotExportSource
ScreenshotExportSource::fromScrollingSnapshot(ScreenshotScrollingSnapshot snapshot) {
    ScreenshotExportSource source;
    if (snapshot.isValid()) {
        ScreenshotClipboardAppearance appearance;
        appearance.rasterSize = snapshot.rowSource().size;
        // A scrolling result is an opaque, unstyled raster, just like its direct pin.
        source.m_clipboardAppearance = std::move(appearance);
    }
    source.m_imageProducer = [snapshot](const ScreenshotExportCancellation& cancellation) mutable {
        return cancellation.isCancellationRequested() ? QImage{} : snapshot.materialize();
    };
    source.m_rowSourceFactory = [snapshot](std::function<bool()> cancellationRequested) mutable {
        return snapshot.rowSource(std::move(cancellationRequested));
    };
    return source;
}
