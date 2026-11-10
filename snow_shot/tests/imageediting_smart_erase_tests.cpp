// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QThread>
#include <functional>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void until(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "imported smart erase reconstruction did not complete");
}

void prepareCanvas(SnowCanvasWidget& canvas) {
    canvas.resize(96, 64);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(48, 32, 1), "configure smart erase import camera");
    require(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter),
            "activate imported smart erase creation");
    SnowCanvasFilterStyle style;
    style.type = SnowCanvasFilterType::SmartErase;
    require(canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyType),
            "configure imported smart erase creation");
}

void drag(SnowCanvasWidget& canvas, QPointF start, QPointF end) {
    const auto mouse = [&](QEvent::Type type, QPointF position, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

QImage render(snow_shot::presentation::ImageEditingDocument& document) {
    return document.runtime().renderToImage(document.bounds(), document.image().size(),
                                            {{document.image(), document.bounds()}});
}

ScreenshotExportImageResult exportImage(ScreenshotExportSource source) {
    ScreenshotExportArtifact artifact(std::move(source));
    QObject receiver;
    ScreenshotExportImageResult result;
    int completions = 0;
    require(artifact.requestImage(&receiver,
                                  [&](auto completed) {
                                      result = std::move(completed);
                                      ++completions;
                                  }),
            "smart erase composite export starts");
    until([&] { return completions != 0; });
    require(completions == 1, "smart erase composite completes exactly once");
    return result;
}

void requirePixels(const QImage& actual, const QImage& expected, const char* message) {
    require(!actual.isNull() && actual.size() == expected.size(), message);
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x)
            require(actual.pixelColor(x, y) == expected.pixelColor(x, y), message);
}

void styledPinExportsCompleteReconstruction(
    const snow_shot::presentation::ImageEditingSource& pending,
    const snow_shot::presentation::ImageEditingSource& ready) {
    using namespace snow_shot::presentation;
    const auto preparation = captureScreenshotPinnedTextPreparation();
    for (int appearance = 0; appearance < 3; ++appearance) {
        ScreenshotResultStyle style;
        if (appearance == 0)
            style.cornerRadius = 5;
        else if (appearance == 1) {
            style.shadowWidth = 3;
            style.shadowColor = QColor(0, 0, 0, 180);
        } else
            style.region = QRegion(ready.image.rect()).subtracted(QRect(4, 4, 8, 8));
        auto completedSource = ready;
        completedSource.resultStyle = style;
        const auto expected = exportImage(imageEditingCompositeSource(completedSource));
        require(expected.succeeded(), "completed pin reconstruction exports its appearance");
        for (const bool closed : {false, true}) {
            auto source = pending;
            if (closed) {
                snow_shot::storage::PinnedWindowRecord record;
                record.image = ready.image;
                record.contentCanvasRect = ready.canvasRect;
                record.canvasSession = ready.canvasSession;
                record.resultStyle = encodeScreenshotResultStyle(style);
                source = readImageEditingPinned(std::move(record), preparation);
            } else
                source.resultStyle = style;
            const auto actual = exportImage(closed ? imageEditingExportSource(source)
                                                   : imageEditingCompositeSource(source));
            require(actual.succeeded(), "pending and closed pin erasures reconstruct on export");
            requirePixels(actual.image, expected.image,
                          "styled pin flattening preserves all completed erasure pixels");
            const auto flattened = completeImageEditingComposite(source, actual.image);
            require(flattened.flattened && flattened.canvasSession.isEmpty(),
                    "completed pin composite owns its reconstructed pixels");
        }
    }
}

void exportReconstructionHonorsCancellationAndFailure(
    const snow_shot::presentation::ImageEditingSource& pending,
    const snow_shot::presentation::ImageEditingSource& ready) {
    SnowCanvasRuntime runtime;
    require(runtime.restoreDocumentSession(pending.canvasSession),
            "restore export cancellation fixture");
    const QList<SnowCanvasBaseImageSource> donors{{pending.image, pending.canvasRect}};
    const auto before = runtime.smartEraseSnapshot().cacheKey();
    int cancellationChecks = 0;
    require(!runtime.prepareSmartEraseForExport(pending.smartErase, donors, pending.canvasRect,
                                                [&] { return ++cancellationChecks >= 3; }) &&
                cancellationChecks >= 3 && runtime.smartEraseSnapshot().cacheKey() == before,
            "cancellation interrupts reconstruction without publishing partial pixels");
    require(runtime.prepareSmartEraseForExport(pending.smartErase, donors, pending.canvasRect),
            "cancelled reconstruction can retry with an independent export");
    const auto reconstructed = runtime.renderToImage(pending.canvasRect, pending.image.size(),
                                                     {{pending.image, pending.canvasRect}});
    require(reconstructed.pixelColor(32, 32) == QColor(Qt::white),
            "retry reconstructs rather than freezing an incomplete snapshot");
    require(runtime.prepareSmartEraseForExport(ready.smartErase, {}, ready.canvasRect),
            "completed captured pixels can be reused without reconstructing donor context");

    QImage unavailableContext(8, 8, QImage::Format_ARGB32);
    unavailableContext.fill(Qt::black);
    const QRectF coveredByErasure(26, 26, 8, 8);
    const auto failure =
        exportImage(ScreenshotExportSource::fromPinnedViewport({pending.canvasSession,
                                                                unavailableContext,
                                                                coveredByErasure,
                                                                unavailableContext.size(),
                                                                {},
                                                                {}}));
    require(!failure.succeeded() && !failure.error.isEmpty(),
            "unavailable reconstruction context fails export instead of baking original pixels");
    require(runtime.prepareSmartEraseForExport({}, {}, QRectF(200, 200, 8, 8)),
            "erasures outside the exported canvas need no reconstruction");
}

void viewportExportsPreserveRequestedResolution(
    const snow_shot::presentation::ImageEditingSource& pending,
    const snow_shot::presentation::ImageEditingSource& ready) {
    SnowCanvasRuntime runtime;
    require(runtime.restoreDocumentSession(ready.canvasSession),
            "restore resized viewport export fixture");
    runtime.restoreSmartEraseSnapshot(ready.smartErase);
    for (const QSize size : {QSize(48, 32), QSize(192, 128)}) {
        const auto expected =
            runtime.renderToImage(ready.canvasRect, size, {{ready.image, ready.canvasRect}});
        const auto actual =
            exportImage(ScreenshotExportSource::fromPinnedViewport({pending.canvasSession,
                                                                    pending.image,
                                                                    pending.canvasRect,
                                                                    size,
                                                                    {},
                                                                    pending.smartErase}));
        require(actual.succeeded() && actual.image.size() == size,
                "viewport exports project background pixels to the requested output resolution");
        requirePixels(actual.image, expected,
                      "resized viewport exports retain reconstructed and original pixels");
    }
}

void reconstructionIgnoresVisualOnlySamplingOverlays(
    const snow_shot::presentation::ImageEditingSource& pending,
    const snow_shot::presentation::ImageEditingSource& ready) {
    const auto expected =
        exportImage(ScreenshotExportSource::fromPinnedViewport({ready.canvasSession,
                                                                ready.image,
                                                                ready.canvasRect,
                                                                ready.image.size(),
                                                                {},
                                                                ready.smartErase}));
    require(expected.succeeded() && expected.image.pixelColor(32, 32) == QColor(Qt::white),
            "base-only reconstruction removes the black foreground square");
    QImage overlay(pending.image.size(), QImage::Format_ARGB32);
    overlay.fill(Qt::red);
    ScreenshotPinnedViewportExportSource request{
        pending.canvasSession, pending.image, pending.canvasRect, pending.image.size(), {}, {}};
    request.samplingSources = {{pending.image, pending.canvasRect},
                               {overlay, pending.canvasRect, {}, false}};
    const auto actual = exportImage(ScreenshotExportSource::fromPinnedViewport(request));
    require(actual.succeeded(), "visual-only sampling overlays allow export reconstruction");
    requirePixels(actual.image, expected.image,
                  "visual-only overlays cannot contaminate reconstructed export pixels");
    request.samplingSources.last().smartEraseSource = true;
    const auto eligible =
        exportImage(ScreenshotExportSource::fromPinnedViewport(std::move(request)));
    require(eligible.succeeded() && eligible.image.pixelColor(32, 32) == QColor(Qt::red),
            "eligible overlay control contributes its pixels to reconstruction");
}
} // namespace

void runImageEditingSmartEraseTests() {
    using namespace snow_shot::presentation;
    ImageEditingSource source;
    source.image = QImage(96, 64, QImage::Format_ARGB32);
    source.image.fill(Qt::white);
    {
        QPainter painter(&source.image);
        painter.fillRect(QRect(26, 26, 12, 12), Qt::black);
        painter.fillRect(QRect(58, 26, 12, 12), Qt::black);
    }
    source.canvasRect = QRectF(0, 0, 96, 64);
    SnowCanvasRuntime emptyPin;
    source.canvasSession = emptyPin.serializeDocumentSession();
    source.smartErase = emptyPin.smartEraseSnapshot();
    require(source.smartErase.data != nullptr, "a live pin supplies an allocated empty snapshot");

    ImageEditingDocument original;
    require(original.load(source), "import the empty live pin smart erase snapshot");
    SnowCanvasWidget originalCanvas(original.runtime());
    prepareCanvas(originalCanvas);
    drag(originalCanvas, QPointF(22, 22), QPointF(42, 42));
    const auto pending = original.snapshot();
    until([&] { return render(original).pixelColor(32, 32) == QColor(Qt::white); });
    const auto ready = original.snapshot();
    require(!ready.smartErase.cacheKey().isEmpty(), "pin import tracks newly created erasures");

    ImageEditingDocument imported;
    require(imported.load(ready), "import completed live pin erasures");
    require(imported.runtime().smartEraseSnapshot().cacheKey() == ready.smartErase.cacheKey() &&
                render(imported).pixelColor(32, 32) == QColor(Qt::white),
            "editor import immediately preserves completed erasure pixels");
    SnowCanvasWidget importedCanvas(imported.runtime());
    prepareCanvas(importedCanvas);
    require(imported.runtime().setQuickSelectionDisabledTools({SnowCanvasTool::RectangleFilter}),
            "allow a second imported smart erase without selecting the first");
    require(importedCanvas.resetEditingStatePreservingTool(), "clear imported selection");
    drag(importedCanvas, QPointF(54, 22), QPointF(74, 42));
    until([&] { return render(imported).pixelColor(64, 32) == QColor(Qt::white); });
    require(imported.undo() && render(imported).pixelColor(64, 32) == QColor(Qt::black),
            "undo removes an erasure created after live pin import");
    require(imported.redo(), "redo the erasure created after live pin import");
    until([&] { return render(imported).pixelColor(64, 32) == QColor(Qt::white); });
    {
        SnowCanvasRuntimeEditor editor(imported.runtime());
        require(editor.deleteAllElements(), "delete imported and newly created erasures");
    }
    require(imported.runtime().smartEraseSnapshot().cacheKey().isEmpty() &&
                render(imported).pixelColor(32, 32) == QColor(Qt::black),
            "deleting erasures after import removes their reconstructed appearance");
    require(original.runtime().smartEraseSnapshot().cacheKey() == ready.smartErase.cacheKey(),
            "editing an imported snapshot preserves its donor runtime");

    ImageEditingDocument unfinished;
    require(unfinished.load(pending), "import an in-flight live pin erasure");
    until([&] { return render(unfinished).pixelColor(32, 32) == QColor(Qt::white); });
    require(pending.smartErase.cacheKey() != unfinished.runtime().smartEraseSnapshot().cacheKey(),
            "in-flight imported erasures compute independently of their captured snapshot");

    styledPinExportsCompleteReconstruction(pending, ready);
    exportReconstructionHonorsCancellationAndFailure(pending, ready);
    viewportExportsPreserveRequestedResolution(pending, ready);
    reconstructionIgnoresVisualOnlySamplingOverlays(pending, ready);
}
