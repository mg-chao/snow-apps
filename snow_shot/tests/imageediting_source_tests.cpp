// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotselectioncomposition.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedrenderpreparation.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QApplication>
#include <QColorSpace>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;
using namespace snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void requirePixels(const QImage& actual, const QImage& expected, const char* message) {
    if (actual.isNull() || actual.size() != expected.size())
        throw std::runtime_error(QStringLiteral("%1: actual %2x%3, expected %4x%5")
                                     .arg(QString::fromUtf8(message))
                                     .arg(actual.width())
                                     .arg(actual.height())
                                     .arg(expected.width())
                                     .arg(expected.height())
                                     .toStdString());
    // PNG stores straight 8-bit RGBA. QColor's 16-bit unpremultiplication can differ after that
    // round trip even when every rendered premultiplied byte is identical. Compare those exact
    // rendered bytes, including alpha, without a visual tolerance.
    const auto actualPixels = actual.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto expectedPixels = expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x) {
            const QRgb a = actualPixels.pixel(x, y);
            const QRgb e = expectedPixels.pixel(x, y);
            if (a != e)
                throw std::runtime_error(
                    QStringLiteral("%1: pixel (%2,%3), source formats %4/%5, actual premultiplied "
                                   "ARGB %6, expected %7")
                        .arg(QString::fromUtf8(message))
                        .arg(x)
                        .arg(y)
                        .arg(actual.format())
                        .arg(expected.format())
                        .arg(QString::number(a, 16))
                        .arg(QString::number(e, 16))
                        .toStdString());
        }
}

QImage exportPixels(ScreenshotExportSource source) {
    ScreenshotExportArtifact artifact(std::move(source));
    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool completed = false;
    QImage output;
    require(artifact.requestImage(&receiver,
                                  [&](auto result) {
                                      completed = true;
                                      output = std::move(result.image);
                                      loop.quit();
                                  }),
            "history editor export starts");
    timeout.start(15000);
    if (!completed)
        loop.exec();
    require(completed && !output.isNull(), "history editor export completes");
    return output;
}

QImage exportSource(const ImageEditingSource& source) {
    ImageEditingDocument document;
    require(document.load(source), "history source loads into an independent editing document");
    const auto snapshot = document.snapshot();
    require(snapshot.error.isEmpty(), "history document prepares its export");
    return exportPixels(imageEditingExportSource(snapshot));
}

CaptureHistoryDraft makeDraft(const SnowCanvasRuntime& runtime) {
    CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = QDateTime::currentDateTimeUtc();
    draft.canvasBounds = QRect(-20, 10, 96, 64);
    draft.selection.rectangle = QRect(-10, 18, 64, 40);
    draft.selection.shadowColor = Qt::transparent;
    draft.canvasHistory = runtime.serializeDocumentHistory();
    CaptureHistoryDisplayDraft display;
    display.stableId = QStringLiteral("history-editor-source");
    display.name = QStringLiteral("History editor source");
    display.sourceCanvasRect = draft.canvasBounds;
    display.image = QImage(draft.canvasBounds.size(), QImage::Format_ARGB32_Premultiplied);
    display.image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < display.image.height(); ++y)
        for (int x = 0; x < display.image.width(); ++x)
            display.image.setPixelColor(x, y, QColor((x * 3) % 256, (y * 5) % 256, 71));
    draft.displays.append(std::move(display));
    return draft;
}

QImage composeHistoryResult(SnowCanvasRuntime& runtime, const CaptureHistoryDraft& draft) {
    const auto& selection = draft.selection;
    const auto pixels = selection.rectangle.size();
    const QList<CanvasExportSource> sources{
        {draft.displays.front().image, QRectF(draft.canvasBounds)}};
    ScreenshotResultStyle style{selection.cornerRadius, selection.shadowWidth,
                                selection.shadowColor};
    style.backgroundOpacityPercent = selection.backgroundOpacityPercent;
    if (selection.region)
        style.region = selection.region->translated(-selection.rectangle.topLeft());
    std::optional<ScreenshotLayoutCompactionPlan> plan;
    if (style.region && selection.layoutCompaction.enabled) {
        plan = makeScreenshotLayoutCompactionPlan(
            *style.region, pixels, 1.0, selection.layoutCompaction, selection.shadowWidth);
        require(plan->isValid() && plan->changed, "history fixture compacts separate regions");
        style = compactedScreenshotResultStyle(style, *plan);
    }
    if (style.region && runtime.hasDocumentContent())
        return renderAnnotatedScreenshotSelection(runtime, selection.rectangle, pixels, sources,
                                                  style, plan ? &*plan : nullptr);
    const auto content = runtime.renderToImage(selection.rectangle, pixels, sources);
    const auto background =
        renderScreenshotSelectionBackground(sources, selection.rectangle, pixels, style);
    return ScreenshotResultCompositor::compose(content, style, 1.0, 1.0, background);
}

void appearanceSurvivesHistoryEditing() {
    QTemporaryDir directory;
    require(directory.isValid(), "history appearance fixture directory");
    auto repository = makeCaptureHistoryRepository(directory.path());
    SnowCanvasRuntime runtime;
    require(
        !runtime
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[-2,26,10,6],"style":{"fill":[255,0,0,255],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "history appearance fixture has an editable annotation");
    for (int variant = 0; variant < 7; ++variant) {
        auto draft = makeDraft(runtime);
        const QRect selection = draft.selection.rectangle;
        switch (variant) {
        case 0:
            draft.selection.region = ScreenshotRegionGeometry(QRegion(selection).subtracted(
                QRect(selection.topLeft() + QPoint(25, 12), QSize(12, 10))));
            break;
        case 1: {
            QPainterPath ellipse;
            ellipse.addEllipse(QRectF(selection));
            draft.selection.region =
                ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve);
            break;
        }
        case 2:
            draft.selection.cornerRadius = 10;
            break;
        case 3:
            draft.selection.shadowWidth = 5;
            draft.selection.shadowColor = QColor(0, 0, 0, 180);
            break;
        case 4:
        case 5:
            draft.selection.region = ScreenshotRegionGeometry(
                QRegion(QRect(selection.topLeft(), QSize(20, selection.height()))) +
                QRect(selection.topLeft() + QPoint(44, 0), QSize(20, selection.height())));
            if (variant == 4)
                draft.selection.backgroundOpacityPercent = 50;
            else
                draft.selection.layoutCompaction = {true, 3};
            break;
        default:
            // These preferences alone do not change plain rectangular output.
            draft.selection.backgroundOpacityPercent = 50;
            draft.selection.layoutCompaction = {true, 3};
            draft.selection.lockAspectRatio = true;
            draft.selection.lockDragAspectRatio = true;
            break;
        }
        draft.resultImage = composeHistoryResult(runtime, draft);
        require(!draft.resultImage->isNull(), "history appearance fixture renders");
        const auto published = repository->publish(draft).get();
        require(published.storage.success, "history appearance fixture publishes");
        const auto stored = repository->loadResultImagePreservingRecord(published.record);
        require(stored.has_value(), "history appearance persists its composited pixels");
        requirePixels(*stored, *draft.resultImage,
                      "history PNG preserves the fixture's rendered appearance");
        const auto revision = repository->recordsSnapshot().revision;
        const auto imported = readImageEditingHistory(*repository, published.record);
        require(imported.error.isEmpty(), "history appearance imports successfully");
        require(imported.flattened && imported.canvasHistory.isEmpty() &&
                    imported.canvasSession.isEmpty(),
                "every selection appearance opens its stored result as a new image");
        require(imported.image == *stored,
                "history import preserves the stored decoded composite pixels exactly");
        requirePixels(exportSource(imported), *stored,
                      "history editor export preserves selection appearance without duplicates");
        require(repository->recordsSnapshot().revision == revision,
                "history appearance import does not modify the source record");
    }

    SnowCanvasRuntime empty;
    auto decorated = makeDraft(empty);
    decorated.selection.shadowWidth = 4;
    decorated.selection.shadowColor = QColor(0, 0, 0, 180);
    decorated.resultImage = composeHistoryResult(empty, decorated);
    const auto decoratedRecord = repository->publish(decorated).get();
    require(decoratedRecord.storage.success, "unannotated decorated history fixture publishes");
    const auto storedDecoration =
        repository->loadResultImagePreservingRecord(decoratedRecord.record);
    require(storedDecoration.has_value(), "unannotated history persists its decorated image");
    requirePixels(*storedDecoration, *decorated.resultImage,
                  "history PNG preserves the unannotated fixture's rendered decoration");
    const auto importedDecoration = readImageEditingHistory(*repository, decoratedRecord.record);
    require(importedDecoration.flattened && importedDecoration.canvasHistory.isEmpty(),
            "unannotated history also explicitly imports its complete decorated image");
    require(importedDecoration.image == *storedDecoration,
            "unannotated import preserves the stored decoded decorated pixels exactly");
    requirePixels(exportSource(importedDecoration), *storedDecoration,
                  "unannotated history export preserves its decoration");

    auto unavailable = makeDraft(runtime);
    unavailable.selection.cornerRadius = 10;
    const auto published = repository->publish(std::move(unavailable)).get();
    require(published.storage.success, "history fixture without a composite publishes");
    const auto revision = repository->recordsSnapshot().revision;
    const auto failed = readImageEditingHistory(*repository, published.record);
    require(failed.image.isNull() && !failed.error.isEmpty() && failed.canvasHistory.isEmpty(),
            "missing appearance composite fails without exposing raw excluded pixels");
    require(repository->recordsSnapshot().revision == revision,
            "failed appearance import never removes the source record");
}

void historyImportsStoredResults() {
    QTemporaryDir directory;
    require(directory.isValid(), "history result fixture directory");
    auto repository = makeCaptureHistoryRepository(directory.path());
    SnowCanvasRuntime runtime;
    require(
        !runtime
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[-2,26,10,6],"style":{"fill":[255,0,0,255],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "history result fixture has a baked annotation");
    for (int variant = 0; variant < 5; ++variant) {
        auto draft = makeDraft(runtime);
        if (variant < 3)
            draft.scrolling = variant != 0;
        const auto viewport = composeHistoryResult(runtime, draft);
        QSize size = viewport.size();
        if (variant != 0) {
            if (variant % 2 == 0)
                size.rwidth() *= 4;
            else
                size.rheight() *= 4;
        }
        QImage result(size, QImage::Format_ARGB32_Premultiplied);
        result.setColorSpace(QColorSpace::SRgb);
        result.fill(Qt::green);
        {
            QPainter painter(&result);
            painter.drawImage(QPoint(), viewport);
        }
        draft.resultImage = result;
        const auto published = repository->publish(draft).get();
        require(published.storage.success, "history result fixture publishes");
        const auto revision = repository->recordsSnapshot().revision;
        const auto imported = readImageEditingHistory(*repository, published.record);
        require(imported.error.isEmpty() && imported.flattened &&
                    imported.canvasRect == QRectF(QPointF(), QSizeF(size)) &&
                    imported.pixelSize == size && imported.canvasHistory.isEmpty() &&
                    imported.canvasSession.isEmpty() && imported.samplingSources.isEmpty(),
                "ordinary, scrolling and legacy history open the complete result in pixel space");
        requirePixels(imported.image, result, "history imports the saved result pixels");
        requirePixels(exportSource(imported), result,
                      "history result exports without restoring or duplicating old annotations");
        ImageEditingDocument document;
        require(document.load(imported) && !document.undo() &&
                    !document.runtime().hasDocumentContent(),
                "history result starts a clean document without the old annotation history");
        require(
            !document.runtime()
                    .applyAnnotationTransaction(
                        R"({"version":1,"operations":[{"type":"rectangle","bounds":[2,2,10,6],"style":{"fill":[0,0,255,255],"stroke":[0,0,0,0]}}]})")
                    .isEmpty() &&
                document.undo(),
            "new annotations can be edited and undone independently");
        requirePixels(exportSource(document.snapshot()), result,
                      "undoing new annotations preserves the baked history result");

        const auto assets = repository->displayAssets(published.record);
        require(assets && assets->result && !assets->displays.isEmpty(),
                "history result fixture exposes its persisted assets");
        require(QFile::remove(assets->displays.front().localFileUrl.toLocalFile()) &&
                    QFile::remove(
                        QDir(QFileInfo(assets->result->localFileUrl.toLocalFile()).absolutePath())
                            .filePath(QStringLiteral("canvas_history.json"))),
                "history result fixture removes its old display and canvas payloads");
        const auto withoutPayload = readImageEditingHistory(*repository, published.record);
        require(withoutPayload.error.isEmpty(), "result import does not need the original capture");
        requirePixels(withoutPayload.image, result,
                      "missing original payloads cannot change the imported result");
        require(repository->recordsSnapshot().revision == revision,
                "history result import and editing never modify the original record");
    }
}

void historyImportsRequireStoredResults() {
    QTemporaryDir directory;
    require(directory.isValid(), "missing history result fixture directory");
    auto repository = makeCaptureHistoryRepository(directory.path());
    SnowCanvasRuntime runtime;
    for (int variant = 0; variant < 3; ++variant) {
        auto draft = makeDraft(runtime);
        if (variant != 0)
            draft.resultImage = composeHistoryResult(runtime, draft);
        const auto published = repository->publish(std::move(draft)).get();
        require(published.storage.success, "missing history result fixture publishes");
        const auto assets = repository->displayAssets(published.record);
        require(assets && !assets->displays.isEmpty(), "original history display is retained");
        if (variant == 1) {
            require(assets->result && QFile::remove(assets->result->localFileUrl.toLocalFile()),
                    "history result fixture removes its saved result");
        } else if (variant == 2) {
            require(assets->result.has_value(), "history result fixture has a saved result");
            QFile corrupt(assets->result->localFileUrl.toLocalFile());
            require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("invalid") == 7,
                    "history result fixture corrupts its saved result");
        }
        const auto revision = repository->recordsSnapshot().revision;
        const auto imported = readImageEditingHistory(*repository, published.record);
        require(imported.image.isNull() && !imported.error.isEmpty() &&
                    imported.canvasHistory.isEmpty() && imported.canvasSession.isEmpty(),
                "absent or unreadable results fail without reconstructing the original selection");
        require(QFileInfo::exists(assets->displays.front().localFileUrl.toLocalFile()) &&
                    repository->recordsSnapshot().revision == revision &&
                    repository->records().contains(published.record),
                "failed result imports preserve the original history record and capture");
    }
}

void layeredImportsRetainInteriorCoverage() {
    QImage image(16, 12, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(120, 60, 30, 170));
    const QRectF frame(-3.25, 2.5, 8, 6);
    const QRectF coverage(-1.75, 3.75, 4.5, 3.5);
    const QSize pixels(19, 15);
    const auto source = ScreenshotImageSource::fromLayers({{image, frame, coverage}});
    SnowCanvasRuntime runtime;
    const auto expected = runtime.renderToImage(frame, pixels, {{image, frame, coverage}});
    const auto actual = renderImageEditingBaseImage(source, frame, pixels);
    requirePixels(actual, expected, "editor import retains layer exclusions inside its output");
    require(actual.pixelColor(0, 0).alpha() == 0 &&
                actual.pixelColor(pixels.width() / 2, pixels.height() / 2).alpha() > 0,
            "interior layer coverage exposes only its included pixels");
}

void layeredImportsPreserveResampledExportPixels() {
    SnowCanvasRuntime background;
    SnowCanvasRuntime annotations;
    require(
        !annotations
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[-4,10,3,2],"style":{"fill":[250,30,70,160],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "resampled import fixture has an editable annotation");
    for (const qreal scale : {2.0, 1.25, 1.0}) {
        auto draft = makeDraft(annotations);
        draft.canvasBounds = QRect(-11, 5, 26, 14);
        draft.selection.rectangle = QRect(-7, 8, 16, 9);
        draft.cursorAvailable = draft.cursorVisible = true;
        draft.displays.clear();
        QList<CanvasExportSource> donors;
        QList<ScreenshotImageLayer> layers;
        for (int index = 0; index < 2; ++index) {
            CaptureHistoryDisplayDraft display;
            display.stableId = QString::number(index);
            display.name = display.stableId;
            display.sourceCanvasRect = QRect(-11 + index * 13, 5, 13, 14);
            display.canvasUsesPoints = true;
            display.backingScale = index == 0 ? 1.0 : scale;
            const auto size = screenshotSelectionRenderedPixelSize(display.sourceCanvasRect->size(),
                                                                   display.backingScale);
            display.image = QImage(size, QImage::Format_ARGB32);
            display.image.setColorSpace(QColorSpace::SRgb);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    display.image.setPixelColor(
                        x, y,
                        QColor((x * 15 + y * 30) % 255, (x * 45 + index * 90) % 255,
                               (y * 60 + index * 30) % 255, (x + y) % 3 ? 255 : 85));
            const QRectF frame(*display.sourceCanvasRect);
            donors.append({display.image, frame, {}});
            layers.append({display.image, frame, frame.intersected(draft.selection.rectangle)});
            if (index == 0) {
                // A partly clipped cursor exercises source sampling at the selection edges.
                display.cursorPixelRect = QRect(2, 1, 6, 6);
                display.cursorPatch = QImage(6, 6, QImage::Format_ARGB32);
                for (int y = 0; y < 6; ++y)
                    for (int x = 0; x < 6; ++x)
                        display.cursorPatch.setPixelColor(
                            x, y, QColor(255, x * 45, y * 45, (x + y) % 2 ? 255 : 85));
                const QRectF cursor(frame.topLeft() + QPointF(2, 1), QSizeF(6, 6));
                donors.append({display.cursorPatch, cursor, {}});
                layers.append({display.cursorPatch, cursor,
                               cursor.intersected(draft.selection.rectangle), false});
            }
            draft.displays.append(std::move(display));
        }
        const auto bounds = QRectF(draft.selection.rectangle);
        const auto pixels =
            screenshotSelectionRenderedPixelSize(draft.selection.rectangle.size(), scale);
        const auto expectedBase = background.renderToImage(bounds, pixels, donors);
        const auto expected = annotations.renderToImage(bounds, pixels, donors);
        ImageEditingSource layered;
        layered.layeredImage = ScreenshotImageSource::fromLayers(std::move(layers));
        layered.canvasRect = bounds;
        layered.pixelSize = pixels;
        layered.canvasSession = annotations.serializeDocumentSession();
        ImageEditingWindow window;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        bool loaded = false;
        QObject::connect(&window, &ImageEditingWindow::loaded, &loop, [&](bool success) {
            loaded = success;
            loop.quit();
        });
        window.present(std::move(layered));
        timeout.start(15000);
        if (!loaded)
            loop.exec();
        require(loaded, "layered pin source finishes independent editor preparation");
        requirePixels(window.document()->image(), expectedBase,
                      "layered editor imports preserve projection interpolation and cursor alpha");
        requirePixels(
            exportSource(window.document()->snapshot()), expected,
            "layered editor imports retain annotations without baking or resampling twice");
        window.shutdown();
    }
}

void pinnedAppearanceSurvivesEditing() {
    SnowCanvasRuntime runtime;
    require(
        !runtime
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[8,9,8,6],"style":{"fill":[255,0,0,255],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "styled pin fixture has an annotation");
    PinnedWindowRecord record;
    record.image = QImage(96, 64, QImage::Format_ARGB32_Premultiplied);
    record.image.fill(QColor(20, 80, 160));
    record.contentCanvasRect = QRectF(0, 0, 48, 32);
    record.canvasSourceRect = record.contentCanvasRect;
    record.initialWindowSize = QSize(48, 32);
    record.nativeGeometry = QRect(0, 0, 96, 64);
    record.canvasSession = runtime.serializeDocumentSession();
    record.originalFileName = QStringLiteral("styled-pin.png");
    const auto preparation = captureScreenshotPinnedRenderPreparation();
    for (int variant = 0; variant < 4; ++variant) {
        ScreenshotResultStyle style;
        if (variant == 0)
            style.cornerRadius = 5;
        else if (variant == 1) {
            style.shadowWidth = 3;
            style.shadowColor = QColor(0, 0, 0, 180);
        } else {
            // Persisted and live styles use canvas units, even when the image
            // has two source pixels per canvas unit.
            style.region = QRegion(0, 0, 48, 32).subtracted(QRect(16, 10, 9, 8));
            if (variant == 3) {
                style.borderType = ScreenshotSelectionBorderType::Border;
                style.borderWidth = 3;
                style.shadowWidth = 11;
            }
        }
        record.resultStyle = encodeScreenshotResultStyle(style);
        const auto pixels = record.image.size();
        const qreal scale = pixels.width() / record.contentCanvasRect.width();
        const auto expectedStyle = ScreenshotResultCompositor::scaledStyle(style, scale);
        const auto content = runtime.renderToImage(record.contentCanvasRect, pixels,
                                                   {{record.image, record.contentCanvasRect}});
        const auto expected = ScreenshotResultCompositor::compose(content, expectedStyle);
        const auto flattened = readImageEditingPinned(record, preparation);
        require(flattened.error.isEmpty(), "closed styled pin renders before editing import");
        require(flattened.flattened && flattened.canvasSession.isEmpty() &&
                    flattened.canvasHistory.isEmpty() && !flattened.resultStyle &&
                    flattened.recognition.isEmpty() &&
                    flattened.originalFileName == record.originalFileName,
                "styled pin composite discards baked editable state and retains its filename");
        requirePixels(flattened.image, expected, "closed pin retains its mask and effect pixels");
        requirePixels(exportSource(flattened), expected,
                      "styled pin editor export does not duplicate annotations or effects");
        if (variant >= 2) {
            const int padding = expectedStyle.effectWidth();
            require(flattened.image
                            .pixelColor(qRound(20 * scale) + padding, qRound(14 * scale) + padding)
                            .alpha() == 0,
                    "excluded pin pixels remain excluded when opening the editor");
        }
    }
    record.resultStyle.clear();
    const auto plain = readImageEditingPinned(record, preparation);
    require(!plain.resultStyle && !imageEditingCompositeSource(plain).isValid() &&
                plain.canvasSession.isEmpty() && plain.flattened,
            "plain pin annotations also become the independent editor's rendered base");
    record.resultStyle = QByteArrayLiteral("invalid style");
    const auto invalid = readImageEditingPinned(record, preparation);
    require(invalid.image.isNull() && !invalid.error.isEmpty(),
            "invalid pin appearance never falls back to raw donor pixels");
}

void pinnedRasterPreparationUsesExpandedGeometry() {
    PinnedWindowRecord record;
    record.image = QImage(80, 60, QImage::Format_ARGB32_Premultiplied);
    record.image.fill(QColor(20, 80, 160));
    record.image.setPixelColor(5, 5, QColor(20, 80, 160, 128));
    record.canvasSourceRect = record.contentCanvasRect = QRectF(100, 200, 80, 60);
    record.initialWindowSize = QSize(80, 60);
    record.nativeGeometry = QRect(50, 70, 120, 90);
    record.scalePercent = 999; // Informational values cannot override encoded window geometry.
    record.opacityPercent = 60;
    record.originalFileName = QStringLiteral("shared-raster.png");
    const auto preparation = captureScreenshotPinnedRenderPreparation();
    const auto full = renderScreenshotPinnedImageRecord(record, preparation);
    require(full.succeeded() && full.image.size() == QSize(120, 90) &&
                qAbs(full.image.pixelColor(40, 40).alpha() - 153) <= 1,
            "persisted raster uses geometry-derived zoom and final opacity");
    const auto source = readScreenshotPinnedImageSnapshot(record, preparation);
    require(source.error.isEmpty() && source.outputSize == full.image.size() &&
                source.originalFileName == record.originalFileName,
            "deferred pin source reports rendered size and retains naming metadata");
    requirePixels(exportPixels(source.source), full.image,
                  "deferred and worker-side pin rendering share identical pixels");
    record.thumbnailMode = true;
    record.preThumbnailNativeGeometry = record.nativeGeometry;
    record.nativeGeometry.setSize(QSize(16, 12));
    requirePixels(renderScreenshotPinnedImageRecord(record, preparation).image, full.image,
                  "thumbnail resolution never changes the expanded rendered result");
    requirePixels(readImageEditingPinned(record, preparation).image, record.image,
                  "editing a saved thumbnail uses source resolution and opacity");
    record.thumbnailMode = false;
    record.cropRect = QRectF(10.25, 11.5, 30.5, 20.25);
    record.scalePercent = 137;
    const auto baseline =
        screenshotPinnedScaleReferenceSize(record.initialWindowSize, {}, 0, record.cropRect,
                                           record.image.size(), record.canvasSourceRect, {});
    record.nativeGeometry.setSize(
        QSize(qRound(baseline.width() * 1.37), qRound(baseline.height() * 1.37)));
    require(screenshotPinnedRestoredScalePercent(baseline, record.nativeGeometry.size(), 137,
                                                 true) == 137,
            "fractional crop retains precise saved zoom only when geometry agrees");
    const auto cropped = renderScreenshotPinnedImageRecord(record, preparation);
    require(cropped.succeeded() && cropped.image.size() == record.nativeGeometry.size(),
            "fractional persisted crop uses canonical rounding at the chosen zoom");
    int allocations = 0;
    const auto rejected = readScreenshotPinnedImageSnapshot(record, preparation, {}, [&](qint64) {
        ++allocations;
        return false;
    });
    require(allocations > 0 && !rejected.source.isValid() && !rejected.error.isEmpty(),
            "persisted raster respects allocation admission before transformation");
    require(!readScreenshotPinnedImageSnapshot(record, preparation, [] { return true; })
                 .source.isValid(),
            "cancelled record preparation never publishes an export source");
    record.canvasSession = QByteArrayLiteral("invalid current drawing");
    const auto invalid = renderScreenshotPinnedImageRecord(record, preparation);
    require(!invalid.succeeded() && invalid.image.isNull(),
            "invalid drawing state cannot publish unannotated persisted pixels");
}

void pinnedRenderPlansRejectOverflowAndInvalidGeometry() {
    ScreenshotPinnedViewportExportSource source;
    source.backgroundImage = QImage(4, 4, QImage::Format_ARGB32_Premultiplied);
    source.backgroundImage.fill(Qt::blue);
    source.backgroundCanvasRect = QRectF(100, -20, 16, 16);
    constexpr int maximum = std::numeric_limits<int>::max();
    source.contentPixelSize = QSize(maximum, maximum);
    require(!screenshotPinnedViewportRenderPlan(source) &&
                renderScreenshotPinnedViewport(source).isNull(),
            "unrepresentable full-image byte counts fail before allocating virtual rasters");
    source.contentPixelSize = QSize(maximum - 1, 2);
    source.resultStyle.shadowWidth = 4;
    require(!screenshotPinnedViewportRenderPlan(source) &&
                renderScreenshotPinnedViewport(source).isNull(),
            "effect padding cannot overflow the composed output dimensions");
    source.resultStyle = {};
    source.resultStyle.borderType = ScreenshotSelectionBorderType::Border;
    source.resultStyle.borderWidth = 4;
    require(!screenshotPinnedViewportRenderPlan(source),
            "border expansion also rejects an unrepresentable composed layout");

    source.resultStyle = {};
    source.contentPixelSize = QSize(16, 16);
    const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
    const qreal infinity = std::numeric_limits<qreal>::infinity();
    for (const auto& visible :
         {QRectF(101, -19, 0, 2), QRectF(101, -19, -1, 2), QRectF(nan, -19, 2, 2),
          QRectF(101, infinity, 2, 2), QRectF(101, -19, infinity, 2), QRectF(101, -19, 2, nan)}) {
        source.visibleCanvasRect = visible;
        require(!screenshotPinnedViewportRenderPlan(source) &&
                    renderScreenshotPinnedViewport(source).isNull(),
                "invalid and nonfinite crop coordinates never reach raster rendering");
    }

    source.contentPixelSize = QSize(maximum, maximum);
    source.backgroundCanvasRect = QRectF(100, -20, maximum, maximum);
    source.visibleCanvasRect = QRectF(112, -9, 3, 2);
    const auto crop = screenshotPinnedViewportRenderPlan(source);
    require(crop && !crop->cropAfterRender && crop->renderCanvasRect == *source.visibleCanvasRect &&
                crop->renderPixelSize == QSize(3, 2) && crop->outputSize() == QSize(3, 2) &&
                crop->workingBytes > 0 && crop->workingBytes < 1024,
            "a plain tiny crop admits only its bounded domain despite full-domain byte overflow");
    const auto pixels = renderScreenshotPinnedViewport(source);
    require(!pixels.isNull() && pixels.size() == QSize(3, 2),
            "a valid tiny-crop plan renders without allocating its virtual full image");
    source.resultStyle.cornerRadius = 2;
    require(!screenshotPinnedViewportRenderPlan(source),
            "the same overflowing full domain is rejected when rounded crops must compose it");
}

void pinnedHighDepthTransformsReserveNativeWorkingImages() {
    PinnedWindowRecord record;
    record.image = QImage(16, 12, QImage::Format_RGBA64);
    record.image.fill(QColor(30, 80, 170, 190));
    record.imageTransform.rotate(90);
    record.quarterTurns = 1;
    record.canvasSourceRect = QRectF(-4, 7, 16, 12);
    record.contentCanvasRect = QRectF(-4, 7, 12, 16);
    record.initialWindowSize = record.image.size();
    record.nativeGeometry = QRect(QPoint(), QSize(12, 16));
    const auto preparation = captureScreenshotPinnedRenderPreparation();
    // This admits the previous source + normalized + two output raster estimate, while
    // remaining too small for transformed images that retain the source's 64-bit depth.
    const qint64 capacity = record.image.sizeInBytes() + qint64(16 * 12 * 4) * 3;
    qint64 requested = 0;
    int allocations = 0;
    const auto allocate = [&](qint64 bytes) {
        ++allocations;
        requested = bytes;
        return bytes <= capacity;
    };
    const auto rejected = readScreenshotPinnedImageSnapshot(record, preparation, {}, allocate);
    require(allocations == 1 && requested > capacity && !rejected.source.isValid() &&
                !rejected.error.isEmpty(),
            "high-depth transforms reject insufficient memory before preparing transformed donors");
    allocations = 0;
    const auto rejectedPixels =
        renderScreenshotPinnedImageRecord(record, preparation, {}, allocate);
    require(allocations == 1 && requested > capacity && !rejectedPixels.succeeded() &&
                rejectedPixels.image.isNull(),
            "worker rendering enforces the same native-depth transform admission");
    const auto accepted = renderScreenshotPinnedImageRecord(
        record, preparation, {}, [](qint64 bytes) { return bytes <= 64 * 1024; });
    require(accepted.succeeded(), "sufficient memory admits the high-depth rotated source");
    const auto expected = ScreenshotResultCompositor::normalizeImage(
        snowCanvasTransformImage(record.image, record.imageTransform, Qt::SmoothTransformation));
    requirePixels(accepted.image, expected,
                  "admitted high-depth transforms preserve their normalized rendered pixels");
}

ScreenshotResultStyle pinnedCropAppearance(int variant, QSize size) {
    ScreenshotResultStyle style;
    if (variant == 0)
        style.cornerRadius = 6;
    else if (variant == 1) {
        style.shadowWidth = 4;
        style.shadowColor = QColor(0, 0, 0, 180);
    } else if (variant == 2) {
        style.borderType = ScreenshotSelectionBorderType::Border;
        style.borderWidth = 3;
        style.borderColor = QColor(240, 190, 20);
    } else {
        style.region = QRegion(QRect(QPoint(), size))
                           .subtracted(QRect(size.width() / 4, size.height() / 4, size.width() / 2,
                                             size.height() / 3));
    }
    return style;
}

void setPinnedCropGeometry(PinnedWindowRecord& record, const ScreenshotResultStyle& style,
                           double zoom) {
    record.resultStyle = encodeScreenshotResultStyle(style);
    record.scalePercent = zoom;
    const auto baseline = screenshotPinnedScaleReferenceSize(
        record.initialWindowSize, record.exactInitialWindowSize, record.quarterTurns,
        record.cropRect, record.image.size(), record.canvasSourceRect, style);
    record.nativeGeometry = QRect(QPoint(), QSize(qRound(baseline.width() * zoom / 100),
                                                  qRound(baseline.height() * zoom / 100)));
}

void pinnedStyledCropsReserveTheirFullRenderingWorkingSet() {
    PinnedWindowRecord record;
    record.image = QImage(120, 90, QImage::Format_ARGB32_Premultiplied);
    record.image.fill(QColor(20, 80, 160));
    record.canvasSourceRect = record.contentCanvasRect = QRectF(300, -200, 1200, 900);
    record.initialWindowSize = QSize(1200, 900);
    record.exactInitialWindowSize = QSizeF(record.initialWindowSize);
    record.cropRect = QRectF(3, 4, 1, 1);
    const auto preparation = captureScreenshotPinnedRenderPreparation();
    constexpr qint64 capacity = 1024 * 1024;
    qint64 requested = 0;
    const auto allocate = [&](qint64 bytes) {
        requested = std::max(requested, bytes);
        return bytes <= capacity;
    };
    setPinnedCropGeometry(record, {}, 1000);
    const auto plain = readScreenshotPinnedImageSnapshot(record, preparation, {}, allocate);
    require(plain.error.isEmpty() && plain.source.isValid() && requested <= capacity &&
                !plain.outputSize.isEmpty() &&
                qint64(plain.outputSize.width()) * plain.outputSize.height() * 4 < capacity,
            "a small plain crop is admitted independently of its large virtual full image");
    const auto plainPixels = renderScreenshotPinnedImageRecord(record, preparation, {}, allocate);
    require(plainPixels.succeeded() && plainPixels.image.size() == plain.outputSize,
            "plain stored crops render within their bounded admission");
    requirePixels(exportPixels(plain.source), plainPixels.image,
                  "admitted plain crop snapshots retain their exact bounded rendering");

    for (int variant = 0; variant < 4; ++variant) {
        setPinnedCropGeometry(
            record, pinnedCropAppearance(variant, record.contentCanvasRect.size().toSize()), 1000);
        requested = 0;
        const auto rejected = readScreenshotPinnedImageSnapshot(record, preparation, {}, allocate);
        require(requested > capacity && !rejected.source.isValid() && !rejected.error.isEmpty(),
                "styled crop snapshots reject full-size intermediates before publishing a source");
        requested = 0;
        const auto rejectedPixels =
            renderScreenshotPinnedImageRecord(record, preparation, {}, allocate);
        require(requested > capacity && !rejectedPixels.succeeded() &&
                    rejectedPixels.image.isNull() && !rejectedPixels.error.isEmpty(),
                "stored styled crop workers reject the same oversized rendering working set");
    }

    int allocations = 0;
    const auto cancelled = readScreenshotPinnedImageSnapshot(
        record, preparation, [] { return true; },
        [&](qint64) {
            ++allocations;
            return true;
        });
    require(allocations == 0 && !cancelled.source.isValid() && !cancelled.error.isEmpty(),
            "already cancelled styled crops never request allocation or publish deferred work");
    bool stop = false;
    const auto interrupted = readScreenshotPinnedImageSnapshot(
        record, preparation, [&] { return stop; },
        [&](qint64) {
            if (++allocations == 2)
                stop = true;
            return true;
        });
    require(allocations == 2 && !interrupted.source.isValid() && !interrupted.error.isEmpty(),
            "cancellation during admission prevents publication of a styled crop source");
}

void pinnedStyledCropsMatchFullRenderingWithTheirOriginalCoordinates() {
    SnowCanvasRuntime drawing;
    require(
        !drawing
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[-10,24,12,14],"style":{"fill":[255,0,0,255],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "rotated crop fixture contains a drawing across a region exclusion");
    PinnedWindowRecord record;
    record.image = QImage(48, 36, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < record.image.height(); ++y)
        for (int x = 0; x < record.image.width(); ++x)
            record.image.setPixelColor(x, y, QColor(x * 5, y * 7, 90, (x + y) % 3 ? 255 : 120));
    record.canvasSourceRect = QRectF(201, -105, 48, 36);
    record.contentCanvasRect = QRectF(-17, 23, 36, 48);
    record.initialWindowSize = record.image.size();
    record.exactInitialWindowSize = QSizeF(record.initialWindowSize);
    record.imageTransform.rotate(90);
    record.quarterTurns = 1;
    record.cropRect = QRectF(0, 0, 24, 36);
    record.opacityPercent = 62;
    record.canvasSession = drawing.serializeDocumentSession();
    const auto preparation = captureScreenshotPinnedRenderPreparation();
    const QImage oriented = ScreenshotResultCompositor::normalizeImage(
        snowCanvasTransformImage(record.image, record.imageTransform, Qt::SmoothTransformation));
    QTransform pixelsToCanvas;
    pixelsToCanvas.translate(record.contentCanvasRect.left(), record.contentCanvasRect.top());
    pixelsToCanvas.scale(record.contentCanvasRect.width() / oriented.width(),
                         record.contentCanvasRect.height() / oriented.height());
    const QTransform orientation =
        QImage::trueMatrix(record.imageTransform, record.image.width(), record.image.height());
    const auto visible = (orientation * pixelsToCanvas).mapRect(*record.cropRect);
    constexpr qint64 capacity = 4 * 1024 * 1024;
    for (int variant = 0; variant < 4; ++variant) {
        const auto style = pinnedCropAppearance(variant, record.contentCanvasRect.size().toSize());
        setPinnedCropGeometry(record, style, 175);
        const qreal scale =
            screenshotPinnedRenderScale(175, oriented.size(), record.contentCanvasRect);
        const QSize pixels(qRound(record.contentCanvasRect.width() * scale),
                           qRound(record.contentCanvasRect.height() * scale));
        ScreenshotPinnedViewportExportSource full;
        full.documentSession = record.canvasSession;
        full.backgroundImage = oriented;
        full.backgroundCanvasRect = record.contentCanvasRect;
        full.contentPixelSize = pixels;
        full.resultStyle = ScreenshotResultCompositor::scaledStyle(style, scale);
        full.outputOpacity = record.opacityPercent / 100.0;
        const auto rendered = renderScreenshotPinnedViewport(full);
        const auto layout = ScreenshotResultCompositor::layoutForContent(pixels, full.resultStyle);
        const qreal sx = pixels.width() / record.contentCanvasRect.width();
        const qreal sy = pixels.height() / record.contentCanvasRect.height();
        const QRect crop(
            qRound((visible.x() - record.contentCanvasRect.x()) * sx) - layout.outputRect.x(),
            qRound((visible.y() - record.contentCanvasRect.y()) * sy) - layout.outputRect.y(),
            qRound(visible.width() * sx), qRound(visible.height() * sy));
        require(!rendered.isNull() && rendered.rect().contains(crop),
                "the styled crop reference remains inside its full rendered image");
        const auto expected = rendered.copy(crop);
        if (variant == 3) {
            const QPoint drawingPixel(qRound((-6 - record.contentCanvasRect.left()) * sx),
                                      qRound((36 - record.contentCanvasRect.top()) * sy));
            auto pristine = full;
            pristine.documentSession.clear();
            const auto excluded = renderScreenshotPinnedViewport(pristine);
            const auto annotated = expected.pixelColor(drawingPixel - crop.topLeft());
            require(!excluded.isNull() && excluded.pixelColor(drawingPixel).alpha() == 0 &&
                        annotated.red() > 240 && annotated.green() < 15 &&
                        qAbs(annotated.alpha() - 158) <= 1,
                    "cropped region exclusions preserve overlaid drawing and final opacity");
        }
        qint64 requested = 0;
        const auto allocate = [&](qint64 bytes) {
            requested = std::max(requested, bytes);
            return bytes <= capacity;
        };
        const auto snapshot = readScreenshotPinnedImageSnapshot(record, preparation, {}, allocate);
        require(snapshot.error.isEmpty() && snapshot.source.isValid() &&
                    snapshot.outputSize == expected.size() && requested > 0 &&
                    requested <= capacity,
                "moderate styled crops admit a complete drawing and effect working set");
        requirePixels(
            exportPixels(snapshot.source), expected,
            "deferred styled crops retain full-render phase, origin, rotation and opacity");
        const auto actual = renderScreenshotPinnedImageRecord(record, preparation, {}, allocate);
        require(actual.succeeded(), "admitted styled crop workers render successfully");
        requirePixels(actual.image, expected,
                      "stored styled crops exactly match clipping their full rendered document");
    }
}

void pinnedRecognitionUsesSavedImageVisibility() {
    PinnedWindowRecord record;
    record.image = QImage(100, 60, QImage::Format_ARGB32_Premultiplied);
    record.image.fill(Qt::white);
    record.canvasSourceRect = record.contentCanvasRect = QRectF(0, 0, 100, 60);
    record.initialWindowSize = record.image.size();
    record.nativeGeometry = QRect(QPoint(), record.image.size());
    auto preparation = captureScreenshotPinnedRenderPreparation();
    preparation.backgroundFill = true;
    const auto original = renderScreenshotPinnedImageRecord(record, preparation);
    require(original.succeeded(), "legacy pin without visibility metadata renders its image");
    record.recognitionVisible = true;
    require(!renderScreenshotPinnedImageRecord(record, preparation).succeeded(),
            "missing visible recognition content reports failure instead of substituting pixels");
    const QPolygonF quad{QPointF(5, 5), QPointF(95, 5), QPointF(95, 30), QPointF(5, 30)};
    QDataStream stream(&record.recognitionResults, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_11);
    stream << QStringLiteral("persisted-image-recognition") << quint8(1) << quint8(0) << quint8(0)
           << QString() << QRect(0, 0, 100, 60) << qint64(1) << QStringLiteral("Recognized text")
           << qreal(1) << quad << quint8(0);
    const auto recognized = renderScreenshotPinnedImageRecord(record, preparation);
    require(recognized.succeeded() && recognized.image != original.image,
            "visible cached OCR contributes image pixels without starting recognition");
    record.translationVisible = true;
    requirePixels(renderScreenshotPinnedImageRecord(record, preparation).image, recognized.image,
                  "explicit recognition type overrides the legacy translation visibility hint");
    record.recognitionVisible = false;
    requirePixels(renderScreenshotPinnedImageRecord(record, preparation).image, original.image,
                  "hidden cached OCR does not change the original image baseline");
    if constexpr (snow_shot::app::edition::textTranslation) {
        record.recognitionVisible = true;
        record.textType = PinnedTextType::TranslationResults;
        require(!renderScreenshotPinnedImageRecord(record, preparation).succeeded(),
                "a missing visible translation cannot silently fall back to recognized text");
        stream << quint32(0x53535452) << quint8(2) << QRect(0, 0, 100, 60) << qint64(1)
               << QStringLiteral("Translated words") << qreal(1) << quad << quint8(0) << false
               << QVector<QPolygonF>();
        const auto translated = renderScreenshotPinnedImageRecord(record, preparation);
        require(translated.succeeded() && translated.image != recognized.image &&
                    translated.image != original.image,
                "stored translated image content renders from its saved layout and selected type");
    }
}

void pinnedTextUsesCapturedRenderingInputs() {
    const auto previousPalette = QApplication::palette();
    const auto restore = qScopeGuard([&] { QApplication::setPalette(previousPalette); });
    auto themed = previousPalette;
    const QColor background(25, 50, 74);
    themed.setColor(QPalette::Base, background);
    QApplication::setPalette(themed);
    const auto preparation = captureScreenshotPinnedTextPreparation();
    QApplication::setPalette(previousPalette);
    for (const bool html : {false, true}) {
        PinnedWindowRecord record;
        record.sourceKind = PinnedWindowSourceKind::ClipboardText;
        (html ? record.originalHtml : record.originalText) =
            html ? QStringLiteral("<p><b>Legacy</b> text</p>") : QStringLiteral("Legacy text");
        record.firstCreationTextDpi = 1.5;
        const auto imported = readImageEditingPinned(record, preparation);
        require(imported.error.isEmpty() && imported.image.pixelColor(0, 0) == background,
                "legacy text uses the GUI-captured theme even after that theme changes");
    }
    PinnedWindowRecord swatch;
    swatch.sourceKind = PinnedWindowSourceKind::ClipboardText;
    swatch.originalText = QStringLiteral("#336699");
    swatch.textStyle = PinnedTextStyle{};
    swatch.textStyle->colorSwatch = true;
    swatch.textStyle->backgroundColor = Qt::yellow;
    swatch.textStyle->textColor = Qt::red;
    swatch.textStyle->fontFamily = preparation.colorSwatchStyle.font.family();
    swatch.textStyle->fontSize = 16;
    swatch.textStyle->colorSwatchSize = QSize(342, 210);
    swatch.textStyle->maximumWidth = 342;
    swatch.firstCreationTextDpi = 1.5;
    auto expectedStyle = preparation.colorSwatchStyle;
    expectedStyle.font.setFamily(swatch.textStyle->fontFamily);
    const auto values = parsePinnedColorSwatch(swatch.originalText);
    require(values.has_value(), "color swatch fixture parses");
    const auto expected = renderPinnedColorSwatch(*values, expectedStyle, 1.5);
    const auto imported = readImageEditingPinned(swatch, preparation);
    require(imported.error.isEmpty(), "closed color swatch renders with captured theme inputs");
    requirePixels(imported.image,
                  snowCanvasTransformImage(expected.image, {}, Qt::SmoothTransformation),
                  "closed swatch uses the same current theme as normal pin restoration");
}
} // namespace

void runImageEditingSourceTests() {
    appearanceSurvivesHistoryEditing();
    historyImportsStoredResults();
    historyImportsRequireStoredResults();
    layeredImportsRetainInteriorCoverage();
    layeredImportsPreserveResampledExportPixels();
    pinnedAppearanceSurvivesEditing();
    pinnedRasterPreparationUsesExpandedGeometry();
    pinnedRenderPlansRejectOverflowAndInvalidGeometry();
    pinnedHighDepthTransformsReserveNativeWorkingImages();
    pinnedStyledCropsReserveTheirFullRenderingWorkingSet();
    pinnedStyledCropsMatchFullRenderingWithTheirOriginalCoordinates();
    pinnedRecognitionUsesSavedImageVisibility();
    pinnedTextUsesCapturedRenderingInputs();
}
