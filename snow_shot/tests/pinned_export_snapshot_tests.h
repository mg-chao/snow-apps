// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

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
} // namespace
