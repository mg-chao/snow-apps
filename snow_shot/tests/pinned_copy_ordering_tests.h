// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace pinned_copy_ordering_tests {
using Access = ScreenshotPinnedWindowTestAccess;

enum class CopyKind { Image, File, Original };
enum class Replacement { Batch, Image, Text };

void pendingAnnotationText() {
    const snow_shot::storage::ScreenshotSettings settings;
    for (const bool copyFile : {false, true}) {
        require(settings.setCopyImageFileToClipboard(copyFile), "configure annotation copy output");
        PinnedSelectionFixture fixture;
        auto& window = fixture.add({40, 60}, {360, 220});
        Access::editSelectionOffscreen(window, true);
        auto* canvas = window.findChild<SnowCanvasWidget*>();
        auto* controller = window.findChild<ScreenshotPinnedEditController*>();
        require(canvas && controller && controller->toolbarWindow() &&
                    controller->toolbarWindow()->palette()->activateDrawingShortcut(
                        QStringLiteral("text")),
                "activate the inline annotation editor");
        const QPointF position = canvas->rect().center();
        canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonPress, position,
                                           Qt::LeftButton, Qt::LeftButton);
        canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonRelease, position,
                                           Qt::LeftButton, Qt::NoButton);
        QInputMethodEvent input;
        input.setCommitString(QStringLiteral("Pending clipboard annotation"));
        QCoreApplication::sendEvent(canvas, &input);
        require(canvas->hasActiveTextEditing(), "annotation text is still uncommitted");
        int historyChanges = 0;
        const auto historyConnection = QObject::connect(
            canvas, &SnowCanvasWidget::historyStateChanged, &window, [&] { ++historyChanges; });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(historyConnection); });
        QApplication::clipboard()->setText(QStringLiteral("before annotation copy"));
        Access::copyCurrentViewport(window);
        require(!canvas->hasActiveTextEditing() && historyChanges > 0 &&
                    Access::exportArtifact(window),
                "snapshot preparation commits text without cancelling its own copy request");
        translation_tests::waitUntil([&] { return !Access::exportArtifact(window); },
                                     "annotation copy finishes");
        const auto urls = exportedClipboardUrls();
        require(!copyFile || urls.size() == 1, "file copy publishes the annotated image");
        const QImage copied =
            copyFile ? QImage(urls.first().toLocalFile()) : exportedClipboardImage();
        const QImage expected = pinnedExportSnapshotPixels(Access::fileSave(window), window);
        requirePinnedExportPixels(copied, expected, "clipboard contains the committed annotation");
        require(window.persistenceSnapshot().canvasSession.contains("Pending clipboard annotation"),
                "copy preserves the newly committed annotation in the document");
    }
}

void deferredCopiesYieldToBatch() {
    const snow_shot::storage::ScreenshotSettings settings;
    // Exercise requests waiting for materialization, including Original Content's separate path.
    for (const auto kind : {CopyKind::Image, CopyKind::File, CopyKind::Original}) {
        require(settings.setCopyImageFileToClipboard(kind == CopyKind::File),
                "configure delayed copy output");
        for (const bool sameOwner : {false, true}) {
            PinnedSelectionFixture fixture;
            auto& owner = fixture.add({40, 60});
            auto& target = fixture.add({230, 160}, {90, 70});
            auto config = cachedOcrPinConfig(nullptr);
            config.recognitionResults = {};
            config.recognitionVisible = false;
            config.enableEditing = false;
            ScreenshotImageLoadCallback deliver;
            config.imageLoader = [&deliver](QObject*, ScreenshotImageLoadCallback ready) {
                deliver = std::move(ready);
            };
            ScreenshotPinnedWindow lazy;
            lazy.setAttribute(Qt::WA_DeleteOnClose, false);
            const auto close = qScopeGuard([&] { lazy.close(); });
            require(lazy.present(config), "present a delayed copy source");
            translation_tests::waitUntil([&] { return bool(deliver); },
                                         "delayed image loader starts");
            if (kind == CopyKind::Original)
                Access::copyOriginalContent(lazy);
            else
                Access::copyCurrentViewport(lazy);
            Access::batchCopy(sameOwner ? lazy : owner, {&target});
            pinned_batch_test::waitForSave();
            const auto batch = exportedClipboardUrls();
            require(batch.size() == 1 &&
                        QImage(batch.first().toLocalFile()).size() == QSize(90, 70),
                    "the newer batch publishes its target");
            bool snapshotReady = false;
            lazy.requestRenderedImageSnapshot([&](ScreenshotPinnedImageSnapshot snapshot) {
                require(snapshot.source.isValid(), "supersession preserves the source image");
                snapshotReady = true;
            });
            deliver(config.imageSource.materializedImage);
            translation_tests::waitUntil(
                [&] {
                    return snapshotReady && !Access::exportArtifact(lazy) &&
                           ScreenshotExportCoordinator::shared().pendingJobCount() == 0;
                },
                "older materialization and copy preparation settle");
            require(
                exportedClipboardUrls() == batch,
                "older image, file and original copies cannot replace a newer batch on any pin");
        }
    }
}

void latestDeferredCopyCompletes() {
    for (const bool original : {false, true}) {
        auto config = cachedOcrPinConfig(nullptr);
        config.recognitionResults = {};
        config.recognitionVisible = false;
        config.enableEditing = false;
        ScreenshotImageLoadCallback deliver;
        config.imageLoader = [&deliver](QObject*, ScreenshotImageLoadCallback ready) {
            deliver = std::move(ready);
        };
        ScreenshotPinnedWindow window;
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        const auto close = qScopeGuard([&] { window.close(); });
        require(window.present(config), "present the latest deferred copy source");
        translation_tests::waitUntil([&] { return bool(deliver); }, "latest source starts loading");
        QApplication::clipboard()->setText(QStringLiteral("before deferred copy"));
        if (original)
            Access::copyOriginalContent(window);
        else
            Access::copyCurrentViewport(window);
        deliver(config.imageSource.materializedImage);
        translation_tests::waitUntil(
            [&] {
                return !Access::exportArtifact(window) &&
                       exportedClipboardImage().size() ==
                           config.imageSource.materializedImage.size();
            },
            "the latest original or viewport copy survives initial materialization");
        requirePinnedExportPixels(exportedClipboardImage(), config.imageSource.materializedImage,
                                  "the latest deferred copy publishes real source pixels");
    }
}

void renderingCopiesYieldToNewRequests() {
    const snow_shot::storage::ScreenshotSettings settings;
    for (const bool copyFile : {false, true}) {
        require(settings.setCopyImageFileToClipboard(copyFile), "configure deferred rendering");
        // Freeze the old request after its snapshot, before either clipboard encoder completes.
        for (const auto replacement : {Replacement::Batch, Replacement::Image, Replacement::Text}) {
            PinnedSelectionFixture fixture;
            auto& first = fixture.add({40, 60});
            auto& second = fixture.add({230, 160}, {90, 70});
            QTemporaryDir output;
            require(output.isValid() && settings.setImageSaveDirectory(output.path()) &&
                        settings.setAutoSaveAfterExecutionActions(
                            {QStringLiteral("copy_pinned_image")}),
                    "the older copy also owns an independent automatic save");
            std::function<void(QImage)> deliver;
            auto source = ScreenshotExportSource::fromImageLoader(
                [&deliver](QObject*, std::function<void(QImage)> ready) {
                    deliver = std::move(ready);
                    return true;
                });
            Access::copyRenderedImage(
                first, std::make_shared<ScreenshotExportArtifact>(std::move(source)));
            require(settings.setAutoSaveAfterExecutionActions({}),
                    "later copies do not request automatic saving");
            translation_tests::waitUntil([&] { return bool(deliver); },
                                         "the older copy waits for rendered pixels");
            if (replacement == Replacement::Batch) {
                Access::batchCopy(second, {&second});
                pinned_batch_test::waitForSave();
            } else if (replacement == Replacement::Image) {
                Access::copyCurrentViewport(second);
            } else {
                auto config = cachedOcrPinConfig(nullptr);
                config.originalClipboardContent.text = QStringLiteral("newer original text");
                ScreenshotPinnedWindow text;
                Access::restoreOffscreen(text, config);
                Access::copyOriginalContent(text);
            }
            translation_tests::waitUntil([&] { return !Access::exportArtifact(second); },
                                         "the newer copy completes");
            const auto urls = exportedClipboardUrls();
            const auto image = exportedClipboardImage();
            const auto text = QApplication::clipboard()->text();
            if (replacement == Replacement::Text)
                require(text == QStringLiteral("newer original text"),
                        "new text reaches clipboard");
            else if (replacement == Replacement::Batch || copyFile)
                require(urls.size() == 1 &&
                            QImage(urls.first().toLocalFile()).size() == QSize(90, 70),
                        "the newer image file reaches clipboard");
            else
                require(image.size() == QSize(90, 70), "the newer raster reaches clipboard");
            QImage older(120, 80, QImage::Format_ARGB32_Premultiplied);
            older.fill(Qt::red);
            deliver(older);
            translation_tests::waitUntil(
                [&] {
                    return !Access::exportArtifact(first) &&
                           ScreenshotExportCoordinator::shared().pendingJobCount() == 0;
                },
                "superseded rendering and clipboard publication settle");
            require(exportedClipboardUrls() == urls && exportedClipboardImage() == image &&
                        QApplication::clipboard()->text() == text,
                    "late image and file encoders cannot replace a newer batch, image or text");
            requirePinnedExportPixels(QImage(output.filePath(QStringLiteral("OrderedCopy.png"))),
                                      older, "clipboard supersession preserves automatic saving");
        }
    }
}

void pendingBatchYieldsToRecognitionCopy() {
    PinnedSelectionFixture fixture;
    auto& owner = fixture.add({40, 60});
    auto& target = fixture.add({230, 160});
    auto config = cachedOcrPinConfig(nullptr);
    ScreenshotPinnedWindow text;
    const auto close = qScopeGuard([&] { text.close(); });
    Access::restoreOffscreen(text, config);
    auto* session = Access::recognitionOffscreen(text, config);
    require(session && session->active(), "activate cached recognized text");
    Access::batchCopy(owner, {&target});
    Access::copyEditToolbarContent(text);
    const QString copied = QApplication::clipboard()->text();
    require(copied == QStringLiteral("Saved OCR"), "recognized text reaches the clipboard");
    pinned_batch_test::waitForSave();
    require(QApplication::clipboard()->text() == copied && exportedClipboardUrls().isEmpty(),
            "a pending batch cannot replace a later recognized-text copy");
}

void run() {
    const snow_shot::storage::ScreenshotSettings settings;
    const auto oldActions = settings.autoSaveAfterExecutionActions();
    const auto oldName = settings.autoSaveFilenameFormat();
    const auto oldFormat = settings.imageFormat();
    const auto oldDirectory = settings.imageSaveDirectory();
    const auto oldCopyFiles = settings.copyImageFileToClipboard();
    const auto restore = qScopeGuard([&] {
        settings.setAutoSaveAfterExecutionActions(oldActions);
        settings.setAutoSaveFilenameFormat(oldName);
        settings.setImageFormat(oldFormat);
        settings.setImageSaveDirectory(oldDirectory);
        settings.setCopyImageFileToClipboard(oldCopyFiles);
    });
    require(settings.setAutoSaveAfterExecutionActions({}) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("OrderedCopy")) &&
                settings.setImageFormat(QStringLiteral("png")),
            "configure deterministic clipboard exports without automatic saving");
    pendingAnnotationText();
    deferredCopiesYieldToBatch();
    require(settings.setCopyImageFileToClipboard(false), "configure deferred raster copying");
    latestDeferredCopyCompletes();
    renderingCopiesYieldToNewRequests();
    pendingBatchYieldsToRecognitionCopy();
}
} // namespace pinned_copy_ordering_tests
