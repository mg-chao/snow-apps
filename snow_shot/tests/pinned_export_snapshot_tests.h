// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include "snow_shot/presentation/screenshotexportcache.h"

namespace {
QByteArray pinnedExportHistoryStroke(QPointF offset = {}) {
    QJsonArray points;
    for (int i = 0; i < 8192; ++i)
        points.append(QJsonArray{offset.x() + 10.0 + double(i) / 8192 * 100.0,
                                 offset.y() + 40.0 + std::sin(i * 0.07) * 25.0});
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                     {QStringLiteral("operations"),
                                      QJsonArray{QJsonObject{
                                          {QStringLiteral("type"), QStringLiteral("freehand")},
                                          {QStringLiteral("points"), points}}}}})
        .toJson(QJsonDocument::Compact);
}

QImage pinnedExportSnapshotPixels(const std::shared_ptr<ScreenshotExportArtifact>& artifact,
                                  QObject& receiver) {
    bool completed = false;
    ScreenshotExportImageResult result;
    require(artifact && artifact->requestImage(&receiver,
                                               [&](auto value) {
                                                   result = std::move(value);
                                                   completed = true;
                                               }),
            "pinned export snapshot request starts");
    QElapsedTimer deadline;
    deadline.start();
    while (!completed && deadline.elapsed() < 10000)
        waitForUi(1);
    require(completed && result.succeeded(), "pinned export snapshot renders successfully");
    return result.image;
}

void requirePinnedExportPixels(const QImage& image, const QImage& expected, const char* message) {
    auto pixels = image.convertToFormat(expected.format());
    // File encoding may tag untagged source pixels as sRGB without changing the raster.
    pixels.setColorSpace(expected.colorSpace());
    if (pixels != expected) {
        QPoint first(-1, -1);
        qsizetype different = 0;
        for (int y = 0; y < std::min(pixels.height(), expected.height()); ++y)
            for (int x = 0; x < std::min(pixels.width(), expected.width()); ++x)
                if (pixels.pixel(x, y) != expected.pixel(x, y)) {
                    if (different == 0)
                        first = {x, y};
                    ++different;
                }
        qWarning() << "Pinned pixel mismatch" << message << "actual" << pixels.size() << "expected"
                   << expected.size() << "different" << different << "first" << first
                   << "actualPixel" << (first.x() < 0 ? QColor() : pixels.pixelColor(first))
                   << "expectedPixel" << (first.x() < 0 ? QColor() : expected.pixelColor(first))
                   << "format" << pixels.format() << expected.format();
    }
    require(!pixels.isNull() && pixels == expected, message);
}

void pinnedSnapshotMemoryMetadataTracksCachedDocument() {
    using Access = ScreenshotPinnedWindowTestAccess;
    PinnedSelectionFixture fixture;
    auto& window = fixture.add({40, 60});
    const auto empty = pinnedRenderedSnapshot(window);
    const auto emptyCached = pinnedRenderedSnapshot(window);
    require(empty.renderingBytes > 0 && emptyCached.renderingBytes == empty.renderingBytes &&
                emptyCached.outputSize == empty.outputSize,
            "cached empty pinned snapshots retain their render memory plan");
    require(
        !Access::applyAnnotationTransaction(
             window,
             R"({"version":1,"operations":[{"type":"rectangle","bounds":[20,20,12,12],"style":{"stroke":[0,255,0,255],"fill":[0,255,0,255]}}]})")
             .isEmpty(),
        "cached snapshot memory fixture adds committed drawing content");
    const auto annotated = pinnedRenderedSnapshot(window);
    const auto annotatedCached = pinnedRenderedSnapshot(window);
    require(annotated.renderingBytes > empty.renderingBytes &&
                annotatedCached.renderingBytes == annotated.renderingBytes &&
                annotatedCached.outputSize == empty.outputSize,
            "drawing snapshots plan their committed document before caching memory metadata");
}

void pinnedExportCacheRetainsOnlyCompletedOutput() {
    using Access = ScreenshotPinnedWindowTestAccess;
    auto& cache = ScreenshotExportCache::pinnedPixels();
    cache.clear();
    const auto cleanup = qScopeGuard([&] { cache.clear(); });
    QImage held;
    {
        PinnedSelectionFixture fixture;
        auto& window = fixture.add({40, 60});
        Access::setGeneralOpacity(window, 55);
        auto artifact = Access::viewportExport(window);
        require(cache.retainedBytes() == 0, "pending export donors do not enter output retention");
        held = pinnedExportSnapshotPixels(artifact, window);
        require(cache.retainedBytes() == held.sizeInBytes(),
                "cache charges the actual completed pixel storage");
        artifact.reset();
        const auto repeated = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
        require(held.constBits() == repeated.constBits(),
                "completed pixels survive command release and are shared by later commands");
        cache.clear();
        const auto fresh = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
        require(fresh.constBits() != held.constBits() && fresh == held,
                "trimmed export recomputes pixels without invalidating a held consumer");
        require(Access::crop(window, QRectF(10, 20, 40, 30)), "select a small retained crop");
        cache.clear();
        const auto crop = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
        require(cache.retainedBytes() == crop.sizeInBytes() && crop.size() == QSize(40, 30),
                "cropped exports charge final dimensions rather than the full composition");
    }
    require(cache.retainedBytes() == 0 && !held.isNull(),
            "destroying a window releases cached output while external pixels remain valid");
}

void pinnedCapturedPresentationReplacesDonor() {
    QImage original(60, 40, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::red);
    QImage presentation(original.size(), original.format());
    presentation.fill(Qt::transparent);
    {
        QPainter painter(&presentation);
        painter.fillRect(QRect(0, 0, 30, 40), QColor(0, 0, 255, 128));
    }
    ScreenshotPinnedViewportExportSource source{
        {}, original, QRectF(0, 0, 60, 40), original.size(), {}, {}, 0.75};
    source.presentationImage = presentation;
    source.presentationCanvasRect = source.backgroundCanvasRect;
    const auto unannotated = renderScreenshotPinnedViewport(source);
    require(!unannotated.isNull() && unannotated.size() == original.size() &&
                unannotated.pixelColor(45, 20).alpha() == 0,
            "transparent captured presentation pixels replace rather than reveal the donor");
    requireColorNear(unannotated.pixelColor(5, 5), QColor(0, 0, 255, 96), 1,
                     "captured presentation opacity is applied once without blending the donor");

    SnowCanvasRuntime runtime;
    require(
        !runtime
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[10,10,12,12],"style":{"stroke":[0,255,0,255],"fill":[0,255,0,255]}}]})")
             .isEmpty(),
        "captured presentation fixture contains a drawing annotation");
    source.documentSession = runtime.serializeDocumentSnapshot();
    const auto annotated = renderScreenshotPinnedViewport(source);
    require(!annotated.isNull() && annotated.pixelColor(45, 20).alpha() == 0,
            "drawing over a captured presentation preserves its transparent regions");
    requireColorNear(annotated.pixelColor(15, 15), QColor(0, 255, 0, 191), 1,
                     "ordinary annotations compose once before final presentation opacity");
    requireColorNear(annotated.pixelColor(5, 5), QColor(0, 0, 255, 96), 1,
                     "annotations do not double the captured presentation alpha");
    source.visibleCanvasRect = QRectF(30, 0, 30, 40);
    const auto cropped = renderScreenshotPinnedViewport(source);
    require(cropped.size() == QSize(30, 40) && cropped.pixelColor(15, 20).alpha() == 0,
            "cropped captured presentations never recover hidden donor pixels");
}

void pinnedAsyncSnapshotsMaterializeAndCancel() {
    using Access = ScreenshotPinnedWindowTestAccess;
    for (const int outcome : {0, 1, 2, 3}) {
        auto config = cachedOcrPinConfig(nullptr);
        config.recognitionResults = {};
        config.recognitionVisible = false;
        config.enableEditing = false;
        ScreenshotImageLoadCallback deliver;
        config.imageLoader = [&](QObject*, ScreenshotImageLoadCallback ready) {
            deliver = std::move(ready);
        };
        auto window = std::make_unique<ScreenshotPinnedWindow>();
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        require(window->present(config), "present a deferred asynchronous snapshot source");
        translation_tests::waitUntil([&] { return bool(deliver); },
                                     "deferred snapshot source begins loading");
        require(Access::originalImage(*window).isNull() &&
                    !window->stitchingSnapshot().source.isValid(),
                "a deferred pin cannot synchronously export its placeholder");
        int completions = 0;
        ScreenshotPinnedImageSnapshot snapshot;
        window->requestRenderedImageSnapshot([&](ScreenshotPinnedImageSnapshot result) {
            snapshot = std::move(result);
            ++completions;
        });
        require(completions == 0, "an asynchronous snapshot waits for real source pixels");
        if (outcome == 1)
            window->close();
        else if (outcome == 2)
            window.reset();
        if (outcome == 1 || outcome == 2) {
            require(completions == 1 && !snapshot.error.isEmpty() && !snapshot.source.isValid(),
                    "closing or destroying a pending source reports one failed snapshot");
        }
        deliver(outcome == 3 ? QImage() : config.imageSource.materializedImage);
        translation_tests::waitUntil([&] { return completions != 0; },
                                     "deferred snapshot completes after source resolution");
        if (outcome == 0) {
            require(snapshot.error.isEmpty() && snapshot.source.isValid() &&
                        snapshot.outputSize == config.imageSource.materializedImage.size(),
                    "materialization returns a snapshot of the final published image");
            requirePinnedExportPixels(
                pinnedExportSnapshotPixels(
                    std::make_shared<ScreenshotExportArtifact>(snapshot.source), *window),
                config.imageSource.materializedImage,
                "asynchronous snapshot renders final pixels rather than placeholder content");
        } else {
            require(!snapshot.error.isEmpty() && !snapshot.source.isValid(),
                    "failed or obsolete materialization cannot publish an image snapshot");
        }
        waitForUi(20);
        require(completions == 1, "late materialization never completes a snapshot twice");
    }
}

void pinnedRecognitionSerializationIsAsynchronousAndCancellable() {
    using Access = ScreenshotPinnedWindowTestAccess;
    QTemporaryDir directory;
    QObject blockerOwner;
    for (const bool closeEarly : {false, true}) {
        auto config = cachedOcrPinConfig(nullptr);
        config.recognitionResults = {};
        config.recognitionVisible = false;
        auto window = std::make_unique<ScreenshotPinnedWindow>();
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        require(window->present(config), "present recognition export lifetime fixture");
        waitForUi(100);
        QSemaphore release;
        std::atomic_bool entered = false;
        bool blockerFinished = false;
        const auto unblock = qScopeGuard([&] { release.release(); });
        const auto blocker = ScreenshotExportCoordinator::shared().submit(
            &blockerOwner, ScreenshotExportCoordinator::Priority::Foreground,
            [&](const ScreenshotExportCancellation&) {
                entered = true;
                release.acquire();
                return ScreenshotExportTaskResult{};
            },
            [&](ScreenshotExportTaskResult) { blockerFinished = true; },
            std::numeric_limits<qint64>::max());
        require(blocker.isValid(), "admit exclusive fixture job");
        translation_tests::waitUntil([&] { return entered.load(); }, "exclusive job begins");
        const QString path = directory.filePath(closeEarly ? QStringLiteral("closed.txt")
                                                           : QStringLiteral("frozen.txt"));
        ScreenshotRecognitionFileSnapshot snapshot{ScreenshotRecognitionFileKind::Qr,
                                                   QStringLiteral("frozen draft")};
        Access::saveRecognition(*window, snapshot, path);
        snapshot.source = QStringLiteral("later edit");
        require(Access::recognitionSavePending(*window) && !QFileInfo::exists(path),
                "recognition save returns before worker admission and publication");
        if (closeEarly)
            window.reset();
        release.release();
        translation_tests::waitUntil(
            [&] {
                return blockerFinished && (!window || !Access::recognitionSavePending(*window));
            },
            "recognition workers finish after admission");
        if (closeEarly)
            require(!QFileInfo::exists(path),
                    "destroying a waiting recognition owner cancels publication");
        else {
            QFile file(path);
            require(!Access::recognitionSavePending(*window) && file.open(QIODevice::ReadOnly) &&
                        file.readAll() == "frozen draft",
                    "recognition export uses request-time draft");
        }
    }
}

void pinnedExportsShareRenderedPixels() {
    using Access = ScreenshotPinnedWindowTestAccess;
    const snow_shot::storage::ScreenshotSettings settings;
    const auto oldManualName = settings.manualSaveFilenameFormat();
    const auto oldManualFormat = settings.lastManualSaveFormat();
    const auto oldManualDirectory = settings.lastManualSaveDirectory();
    const auto oldAutomaticName = settings.autoSaveFilenameFormat();
    const auto oldFormat = settings.imageFormat();
    const auto oldDirectory = settings.imageSaveDirectory();
    const auto restore = qScopeGuard([&] {
        settings.setManualSaveFilenameFormat(oldManualName);
        settings.setLastManualSaveFormat(oldManualFormat);
        settings.setLastManualSaveDirectory(oldManualDirectory);
        settings.setAutoSaveFilenameFormat(oldAutomaticName);
        settings.setImageFormat(oldFormat);
        settings.setImageSaveDirectory(oldDirectory);
    });
    for (const bool cropped : {false, true}) {
        PinnedSelectionFixture fixture;
        auto& window = fixture.add({40, 60});
        require(
            !Access::applyAnnotationTransaction(
                 window,
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[20,20,40,30],"style":{"stroke":[255,0,255,255],"fill":[255,0,255,255]}}]})")
                 .isEmpty(),
            "shared export fixture contains a persistent annotation");
        Access::transformReplacement(window);
        Access::applyFractionalScale(window, 137.5);
        Access::setGeneralOpacity(window, 55);
        if (cropped)
            require(Access::crop(window, QRectF(10, 20, 40, 60)),
                    "shared export fixture selects a transformed source crop");
        const auto history = Access::drawingHistory(window);
        const auto expected = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
        require(expected.size() == (cropped ? QSize(55, 83) : QSize(110, 165)),
                "shared output applies fractional zoom after transform and crop");
        const auto shared = pinnedRenderedSnapshot(window);
        require(shared.outputSize == expected.size(),
                "the common snapshot predicts its final pixel dimensions");
        for (const auto& artifact :
             {Access::fileSave(window),
              std::make_shared<ScreenshotExportArtifact>(window.stitchingSnapshot().source),
              std::make_shared<ScreenshotExportArtifact>(window.splittingSnapshot().source),
              std::make_shared<ScreenshotExportArtifact>(shared.source)})
            requirePinnedExportPixels(pinnedExportSnapshotPixels(artifact, window), expected,
                                      "save, stitch, split and asynchronous snapshot match copy");

        const auto comparePersisted = [&] {
            const auto record = window.persistenceSnapshot();
            const auto restored = snow_shot::presentation::readScreenshotPinnedImageSnapshot(
                record, captureScreenshotPinnedRenderPreparation(&window));
            if (restored.outputSize != expected.size() || !restored.error.isEmpty())
                qWarning() << "Persisted render mismatch" << "actual" << restored.outputSize
                           << "expected" << expected.size() << "error" << restored.error << "canvas"
                           << record.canvasSourceRect << "content" << record.contentCanvasRect
                           << "surface" << record.surfaceCanvasRect << "initial"
                           << record.initialWindowSize << "exact" << record.exactInitialWindowSize
                           << "geometry" << record.nativeGeometry << "placement"
                           << record.placement.windowSize << "expanded"
                           << record.preThumbnailNativeGeometry << "zoom" << record.scalePercent
                           << "crop" << record.cropRect.value_or(QRectF()) << "turns"
                           << record.quarterTurns << "transform" << record.imageTransform;
            require(restored.error.isEmpty() && restored.source.isValid() &&
                        restored.outputSize == expected.size(),
                    "persisted state predicts the same transformed, cropped and zoomed output");
            requirePinnedExportPixels(
                pinnedExportSnapshotPixels(
                    std::make_shared<ScreenshotExportArtifact>(restored.source), window),
                expected, "persisted rendering matches the live pin clipboard raster exactly");
        };
        comparePersisted();

        Access::thumbnailForHideTest(window, true);
        const auto thumbnailGeometry = window.currentNativeGeometry();
        for (const auto& artifact :
             {Access::viewportExport(window), Access::fileSave(window),
              std::make_shared<ScreenshotExportArtifact>(window.stitchingSnapshot().source),
              std::make_shared<ScreenshotExportArtifact>(window.splittingSnapshot().source)})
            requirePinnedExportPixels(pinnedExportSnapshotPixels(artifact, window), expected,
                                      "thumbnail display resolution never changes export pixels");
        require(window.persistenceSnapshot().thumbnailMode &&
                    window.currentNativeGeometry() == thumbnailGeometry,
                "exporting an expanded-resolution thumbnail leaves its display state intact");
        comparePersisted();

        snow_shot::presentation::ImageEditingWindow editor;
        bool loaded = false;
        QObject::connect(&editor, &snow_shot::presentation::ImageEditingWindow::loaded, &editor,
                         [&](bool success) {
                             require(success, "shared pin raster opens in the editor");
                             loaded = true;
                         });
        editor.presentSource(shared.source);
        translation_tests::waitUntil([&] { return loaded; }, "shared editor baseline loads");
        requirePinnedExportPixels(editor.document()->image(), expected,
                                  "editing starts with the shared rendered image");
        require(editor.document()->bounds() == QRectF(QPointF(), QSizeF(expected.size())) &&
                    !editor.document()->runtime().hasDocumentContent() &&
                    !editor.document()->runtime().canUndo(),
                "old annotations are baked into a fresh zero-origin editor document");
        require(
            !editor.document()
                 ->runtime()
                 .applyAnnotationTransaction(
                     R"({"version":1,"operations":[{"type":"rectangle","bounds":[2,2,12,10],"style":{"stroke":[0,255,0,255],"fill":[0,255,0,255]}}]})")
                 .isEmpty(),
            "new annotations remain editable over the imported raster");
        const auto edited = pinnedExportSnapshotPixels(
            std::make_shared<ScreenshotExportArtifact>(editor.document()->exportSource()), window);
        require(edited != expected && edited.pixelColor(6, 6) == QColor(Qt::green) &&
                    editor.document()->undo(),
                "new editing composes once and can be undone independently");
        requirePinnedExportPixels(
            pinnedExportSnapshotPixels(
                std::make_shared<ScreenshotExportArtifact>(editor.document()->exportSource()),
                window),
            expected, "undo restores the exact baked baseline");
        editor.shutdown();

        QTemporaryDir output;
        require(output.isValid() &&
                    settings.setManualSaveFilenameFormat(QStringLiteral("SharedBatch")) &&
                    settings.setLastManualSaveFormat(QStringLiteral("png")) &&
                    settings.setAutoSaveFilenameFormat(QStringLiteral("SharedQuick")) &&
                    settings.setImageFormat(QStringLiteral("png")) &&
                    settings.setImageSaveDirectory(output.path()),
                "shared file routes use deterministic PNG destinations");
        Access::batchSave(window, output.path());
        pinned_batch_test::waitForSave();
        requirePinnedExportPixels(
            QImage(output.filePath(QStringLiteral("SharedBatch_1.png"))), expected,
            "batch file encoding consumes the same expanded-resolution thumbnail raster");
        Access::quickSave(window);
        const auto quickPath = output.filePath(QStringLiteral("SharedQuick.png"));
        translation_tests::waitUntil([&] { return !QImage(quickPath).isNull(); },
                                     "Quick Save produces the shared PNG");
        requirePinnedExportPixels(QImage(quickPath), expected,
                                  "Quick Save encoding consumes the same rendered image");
        require(Access::drawingHistory(window) == history,
                "all shared export consumers preserve the source pin history");
        Access::setGeneralOpacity(window, 100);
        requirePinnedExportPixels(
            pinnedExportSnapshotPixels(std::make_shared<ScreenshotExportArtifact>(shared.source),
                                       window),
            expected, "an owned snapshot remains frozen after the source appearance changes");
    }
}

void pinnedPersistedExportsPreserveTextMarks() {
    using Access = ScreenshotPinnedWindowTestAccess;
    pinned_quick_text_annotation_tests::SettingsGuard settings;
    for (const int kind : {0, 2}) {
        pinned_quick_text_annotation_tests::Fixture fixture(kind);
        fixture.drag();
        fixture.click(ScreenshotTextAnnotationTool::Highlight);
        fixture.click(ScreenshotTextAnnotationTool::Underline);
        fixture.content->clearTextSelection();
        require(!Access::quickTextSnapshot(fixture.window).empty(),
                "persisted rendering fixture has persistent selected-text marks");
        Access::rotateFormattedSelectionOffscreen(fixture.window);
        Access::applyFractionalScale(fixture.window, 83.25);
        Access::setGeneralOpacity(fixture.window, 70);
        const auto full = Access::visibleCrop(fixture.window);
        require(Access::crop(fixture.window, full.adjusted(full.width() / 8, full.height() / 8,
                                                           -full.width() / 8, -full.height() / 8)),
                "persisted rendering fixture crops rotated text marks");
        const auto expected =
            pinnedExportSnapshotPixels(Access::viewportExport(fixture.window), fixture.window);
        for (const bool thumbnail : {false, true}) {
            Access::thumbnailForHideTest(fixture.window, thumbnail);
            const auto stored = snow_shot::presentation::readScreenshotPinnedImageSnapshot(
                fixture.window.persistenceSnapshot(),
                captureScreenshotPinnedRenderPreparation(&fixture.window));
            require(stored.error.isEmpty() && stored.outputSize == expected.size(),
                    "saved text marks retain the expanded crop dimensions");
            const auto restored = pinnedExportSnapshotPixels(
                std::make_shared<ScreenshotExportArtifact>(stored.source), fixture.window);
            if (restored != expected) {
                const auto record = fixture.window.persistenceSnapshot();
                qWarning() << "Saved text marks mismatch" << "kind" << kind << "thumbnail"
                           << thumbnail << "dpi" << record.firstCreationTextDpi << "source"
                           << record.canvasSourceRect << "content" << record.contentCanvasRect
                           << "crop" << record.cropRect.value_or(QRectF()) << "transform"
                           << record.imageTransform << "zoom" << record.scalePercent;
            }
            requirePinnedExportPixels(
                restored, expected,
                "persisted formatted and OCR text marks match their live clipboard rendering");
        }
    }
}

void pinnedRecognitionExportsIncludePersistentAnnotations() {
    using Access = ScreenshotPinnedWindowTestAccess;
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load an offscreen font for shared recognition rendering");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    const snow_shot::storage::TextRecognitionSettings settings;
    const auto previous = settings.saveRecognitionResultAsImage();
    const auto restore = qScopeGuard([&] { settings.setSaveRecognitionResultAsImage(previous); });
    require(settings.setSaveRecognitionResultAsImage(false),
            "capture-only recognition saving is disabled for the common pin renderer");
    ScreenshotPinnedWindow window;
    auto config = cachedOcrPinConfig(nullptr);
    config.recognitionResults.translatedText =
        std::make_shared<ScreenshotOcrPresentation>(*config.recognitionResults.text->presentation);
    config.recognitionResults.translatedText->setLineText(0, QStringLiteral("Translated result"));
    Access::restoreOffscreen(window, config);
    require(
        !Access::applyAnnotationTransaction(
             window,
             R"({"version":1,"operations":[{"type":"rectangle","bounds":[650,370,20,20],"style":{"stroke":[255,0,255,255],"fill":[255,0,255,255]}}]})")
             .isEmpty(),
        "recognition fixture contains an ordinary drawing annotation");
    auto* session = Access::recognitionOffscreen(window, config);
    Access::setFractionalScale(window, 125);
    Access::setGeneralOpacity(window, 60);
    const auto shared = pinnedRenderedSnapshot(window);
    const auto expected = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
    require(expected.size() == QSize(400, 225), "active recognition uses the common chosen zoom");
    requireColorNear(expected.pixelColor(20, 20), QColor(255, 0, 255, 153), 1,
                     "active recognition retains ordinary annotations and applies opacity once");
    for (const auto& artifact :
         {Access::fileSave(window),
          std::make_shared<ScreenshotExportArtifact>(window.stitchingSnapshot().source),
          std::make_shared<ScreenshotExportArtifact>(window.splittingSnapshot().source)})
        requirePinnedExportPixels(pinnedExportSnapshotPixels(artifact, window), expected,
                                  "all image routes include recognition and persistent drawings");
    require(session->activateCachedTextTranslation(),
            "activate the cached translated presentation");
    const auto translated = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
    require(translated != expected, "translation changes the shared rendered presentation");
    requirePinnedExportPixels(
        pinnedExportSnapshotPixels(std::make_shared<ScreenshotExportArtifact>(shared.source),
                                   window),
        expected, "later recognition changes cannot alter an owned image snapshot");
    requireColorNear(translated.pixelColor(20, 20), QColor(255, 0, 255, 153), 1,
                     "translation retains the same ordinary drawing annotation");
}

void pinnedExportsIgnoreAnnotationHistory() {
    using Access = ScreenshotPinnedWindowTestAccess;
    PinnedSelectionFixture fixture;
    auto& window = fixture.add({40, 60});
    auto& runtime = Access::drawingRuntime(window);
    const auto stroke = pinnedExportHistoryStroke();
    for (int round = 0; round < 24; ++round) {
        require(!runtime.applyAnnotationTransaction(stroke).isEmpty(),
                "create a large historical annotation");
        SnowCanvasRuntimeEditor editor(runtime);
        require(editor.deleteAllElements(), "delete the historical annotation");
    }
    require(runtime.serializeDocumentSession().isEmpty() && !runtime.hasDocumentContent() &&
                runtime.canUndo(),
            "deleted annotations exceed the full-session limit without changing current content");
    require(pinnedStitchingPixels(window.stitchingSnapshot(), window) ==
                Access::displayImage(window),
            "a pin with only historical annotations still stitches its original pixels");

    const QByteArray annotation = QByteArrayLiteral(
        R"({"version":1,"operations":[{"type":"arrow","points":[[20,20],[100,60]],"style":{"stroke":[255,0,255,255],"stroke_width":6}}]})");
    require(!runtime.applyAnnotationTransaction(annotation).isEmpty(),
            "add an arrow after deleting the old annotations");
    const auto revision = runtime.documentRevision();
    const auto document = runtime.documentCheckpoint();
    const QImage expected =
        runtime.renderToImage(QRectF(0, 0, 120, 80), QSize(120, 80),
                              {{Access::displayImage(window), QRectF(0, 0, 120, 80)}});
    require(!expected.isNull() && expected != Access::displayImage(window),
            "the reference image contains the current arrow");
    for (const auto& artifact :
         {std::make_shared<ScreenshotExportArtifact>(window.stitchingSnapshot().source),
          std::make_shared<ScreenshotExportArtifact>(window.splittingSnapshot().source),
          Access::viewportExport(window), Access::fileSave(window)}) {
        require(
            pinnedExportSnapshotPixels(artifact, window) == expected,
            "stitching, splitting, copying and saving retain annotations beyond the history limit");
    }
    require(runtime.documentRevision() == revision && runtime.documentCheckpoint() == document &&
                runtime.canUndo() && runtime.undo() && !runtime.hasDocumentContent() &&
                runtime.redo() && runtime.documentCheckpoint() == document,
            "export leaves the pin and its undo/redo history intact");
}

void pinnedExportsRejectUnavailableDocumentSnapshots() {
    using Access = ScreenshotPinnedWindowTestAccess;
    PinnedSelectionFixture fixture;
    auto& window = fixture.add({40, 60});
    auto& runtime = Access::drawingRuntime(window);
    // Build valid current content beyond the snapshot limit, away from the viewport.
    // Export must reject it rather than interpreting failed serialization as no annotations.
    SnowCanvasRuntime oversized;
    const auto stroke = pinnedExportHistoryStroke({2000, 2000});
    for (int i = 0; i < 64; ++i)
        require(!oversized.applyAnnotationTransaction(stroke).isEmpty(),
                "create valid content beyond the snapshot limit");
    require(oversized.hasDocumentContent() && oversized.serializeDocumentSnapshot().isEmpty() &&
                runtime.cloneDocumentSessionFrom(oversized),
            "the live pin contains annotations that cannot fit in an export snapshot");
    const auto stitching = window.stitchingSnapshot();
    const auto splitting = window.splittingSnapshot();
    require(!stitching.error.isEmpty() && !stitching.source.isValid() &&
                !splitting.error.isEmpty() && !splitting.source.isValid(),
            "stitching and splitting report snapshot failure instead of losing annotations");
    for (const auto& artifact : {Access::viewportExport(window), Access::fileSave(window)}) {
        bool completed = false;
        ScreenshotExportImageResult result;
        const bool started = artifact && artifact->requestImage(&window, [&](auto value) {
            result = std::move(value);
            completed = true;
        });
        QElapsedTimer deadline;
        deadline.start();
        while (started && !completed && deadline.elapsed() < 10000)
            waitForUi(1);
        require(!started || (completed && !result.succeeded() && result.image.isNull()),
                "copy and save cannot publish unannotated pixels after snapshot failure");
    }
    require(runtime.reset(), "replace content after a rejected export");
    require(pinnedExportSnapshotPixels(Access::viewportExport(window), window) ==
                Access::displayImage(window),
            "a failed snapshot does not poison future cached exports");
}

void pinnedExportsPreserveSamplingDonorEligibility() {
    using Access = ScreenshotPinnedWindowTestAccess;
    QImage original(96, 64, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::white);
    {
        QPainter painter(&original);
        painter.fillRect(QRect(26, 26, 12, 12), Qt::black);
    }
    QImage overlay(original.size(), original.format());
    overlay.fill(Qt::red);
    const QRectF bounds(original.rect());
    SnowCanvasRuntime runtime;
    SnowCanvasWidget editor(runtime);
    editor.resize(original.size());
    editor.show();
    waitForUi(1);
    require(editor.setViewportCamera(48, 32, 1) &&
                editor.setCanvasTool(SnowCanvasTool::RectangleFilter),
            "configure the sampling donor erasure fixture");
    SnowCanvasFilterStyle style;
    style.type = SnowCanvasFilterType::SmartErase;
    require(editor.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyType),
            "configure a smart erasure without a completed reconstruction");
    for (const auto type :
         {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
        const QPointF point = type == QEvent::MouseButtonPress ? QPointF(22, 22) : QPointF(42, 42);
        QMouseEvent event(type, point, editor.mapToGlobal(point.toPoint()),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&editor, &event);
    }
    require(editor.resetEditingState() && runtime.canUndo(), "commit the donor fixture erasure");
    for (const bool eligible : {false, true}) {
        ScreenshotPinnedWindow::Config config;
        config.screen = QGuiApplication::primaryScreen();
        config.nativeGeometry = physicalPinGeometry(*config.screen, {40, 60}, original.size());
        config.initialWindowSize = original.size();
        config.canvasSourceRect = bounds;
        config.imageSource = ScreenshotImageSource::fromImage(overlay, bounds);
        config.initialCanvasSession = runtime.serializeDocumentSession();
        config.initialCanvasSamplingSource = ScreenshotImageSource::fromLayers(
            {{original, bounds, bounds}, {overlay, bounds, bounds, eligible}});
        config.automaticTextRecognition = false;
        ScreenshotPinnedWindow window;
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        bool ready = false;
        require(window.present(config, [&](bool success, QImage) { ready = success; }),
                "present the sampling donor pin");
        translation_tests::waitUntil([&] { return ready; }, "sampling donor pin publishes pixels");
        // Force export reconstruction rather than relying on an interactive worker's timing.
        Access::drawingRuntime(window).restoreSmartEraseSnapshot({});
        const auto live = pinnedExportSnapshotPixels(Access::viewportExport(window), window);
        require(live.pixelColor(32, 32) == QColor(eligible ? Qt::red : Qt::white) &&
                    live.pixelColor(80, 32) == QColor(Qt::red),
                "live exports use eligible donors and keep visual overlays in the backdrop");
        auto record = window.persistenceSnapshot();
        require(record.canvasSamplingImages.size() == 2 &&
                    record.canvasSamplingImages.last().smartEraseSource == eligible,
                "pin persistence retains sampling donor eligibility");
        const auto stored = snow_shot::presentation::renderScreenshotPinnedImageRecord(
            std::move(record), captureScreenshotPinnedRenderPreparation(&window));
        require(stored.succeeded(), "stored sampling pin exports successfully");
        requirePinnedExportPixels(stored.image, live,
                                  "stored and live exports preserve sampling donor eligibility");
    }
}
} // namespace
