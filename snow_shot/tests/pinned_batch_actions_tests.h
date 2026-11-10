// Included after PinnedSelectionFixture in screenshot_pinned_window_tests.cpp.
namespace pinned_batch_test {
using Access = ScreenshotPinnedWindowTestAccess;

QAction& action(ScreenshotPinnedWindow& window, const char* suffix) {
    materializePinnedMenuTree(window);
    auto* menu = window.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedBatchActionsMenu"));
    require(menu != nullptr, "Batch Actions submenu exists");
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    auto* result = menu->findChild<QAction*>(QStringLiteral("screenshotPinnedBatch") +
                                             QString::fromLatin1(suffix));
    require(result != nullptr, "batch command has a stable identity");
    return *result;
}

void waitForSave() {
    QElapsedTimer timer;
    timer.start();
    while (Access::batchSaving() && timer.elapsed() < 15000) {
        waitForUi(5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    require(!Access::batchSaving(), "batch save completes and releases the shared admission guard");
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "batch output is readable");
    return file.readAll();
}

void write(const QString& path, const QByteArray& contents) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(),
            "seed an existing destination");
}

void menuSelectionAndModes() {
    PinnedSelectionFixture fixture;
    auto& first = fixture.add({40, 60});
    auto& second = fixture.add({230, 160});
    auto& thumbnail = fixture.add({420, 160});
    auto& hidden = fixture.add({420, 260});
    auto& inactive = fixture.add({420, 360});
    auto& minimized = fixture.add({520, 360});
    auto& tab = fixture.add({620, 360});
    auto& clickThrough = fixture.add({620, 460});
    hidden.hide();
    minimized.showMinimized();
    const auto otherGroup = fixture.groups.createGroup(QStringLiteral("Batch inactive"));
    require(otherGroup.has_value(), "create an inactive batch group");
    inactive.setGroupId(*otherGroup);
    Access::setLock(second, true);
    Access::thumbnailForHideTest(thumbnail, true);
    require(Access::setClickThrough(clickThrough, true), "seed click-through batch member");
    auto& top = Access::hideToTop(tab);
    require(top.enter(screenshot_pinned_hide_to_top::screenGeometry(tab.screen())),
            "seed a displayed top-edge tab");
    top.animation().setCurrentTime(top.animation().duration());
    ScreenshotPinnedWindow spare;
    const auto targets = Access::batchTargets(first);
    require(targets.size() == 5 && targets.contains(&first) && targets.contains(&second) &&
                targets.contains(&thumbnail) && targets.contains(&tab) &&
                targets.contains(&clickThrough) && !targets.contains(&spare),
            "displayed targets include special modes and exclude hidden/minimized/inactive pins");

    auto& select = action(first, "SelectAllAction");
    auto* menu = first.findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedBatchActionsMenu"));
    const QStringList names = {QStringLiteral("SelectAllAction"),
                               QStringLiteral("ImageStitchingAction"),
                               QStringLiteral("CopyFilesToClipboardAction"),
                               QStringLiteral("SaveAsFileAction"),
                               {},
                               QStringLiteral("EnableThumbnailAction"),
                               QStringLiteral("DisableThumbnailAction"),
                               QStringLiteral("EnableHideToTopAction"),
                               QStringLiteral("DisableHideToTopAction"),
                               QStringLiteral("EnableLockAction"),
                               QStringLiteral("DisableLockAction"),
                               {},
                               QStringLiteral("CloseAction"),
                               QStringLiteral("DestroyAction"),
                               {},
                               QStringLiteral("DisplayedCount")};
    require(menu->actions().size() == names.size(), "batch menu has exactly the requested entries");
    for (qsizetype i = 0; i < names.size(); ++i) {
        auto* item = menu->actions().at(i);
        require(names.at(i).isEmpty()
                    ? item->isSeparator()
                    : item->objectName() == QStringLiteral("screenshotPinnedBatch") + names.at(i),
                "batch menu order and separators match the specification");
    }
    require(
        !first.findChild<QAction*>(QStringLiteral("screenshotPinnedHideAllWindowsToTopAction")) &&
            !action(first, "DisplayedCount").isEnabled() &&
            action(first, "DisplayedCount").text() ==
                ScreenshotPinnedWindow::tr("%n window(s) displayed", nullptr, 5) &&
            menu->actionDanger(&action(first, "DestroyAction")) &&
            !menu->actionDanger(&action(first, "CloseAction")),
        "old action is replaced; count is read-only and only Destroy has danger styling");
    QVector<ScreenshotPinnedImageSnapshot> stitched;
    const auto stitching = QObject::connect(
        &fixture.selection, &PinnedSelectionFixture::Controller::imageStitchingRequested, &first,
        [&](const auto& snapshots) { stitched = snapshots; });
    action(first, "ImageStitchingAction").trigger();
    QElapsedTimer timer;
    timer.start();
    while (stitched.isEmpty() && timer.elapsed() < 10000)
        waitForUi(5);
    require(stitched.size() == targets.size(), "batch stitching includes every displayed mode");
    for (qsizetype i = 0; i < stitched.size(); ++i)
        require(stitched.at(i).label == targets.at(i)->persistenceId() &&
                    stitched.at(i).error.isEmpty() && stitched.at(i).source.isValid(),
                "batch stitching preserves displayed order and rendered snapshots");
    QObject::disconnect(stitching);
    action(first, "SelectAllAction").trigger();
    if (fixture.selection.selectedCount() != 2 || !fixture.selection.isSelected(&first) ||
        !fixture.selection.isSelected(&second) || !second.persistenceSnapshot().lockedMode ||
        !thumbnail.persistenceSnapshot().thumbnailMode || !top.active() ||
        !clickThrough.persistenceSnapshot().clickThroughMode)
        qWarning() << "Batch selection mismatch" << "count" << fixture.selection.selectedCount()
                   << "actionEnabled" << select.isEnabled() << "eligible"
                   << fixture.selection.isSelectable(&first)
                   << fixture.selection.isSelectable(&second) << "first"
                   << fixture.selection.isSelected(&first) << first.isVisible()
                   << first.isMinimized() << Access::shakeContentReady(first) << first.groupId()
                   << "second" << fixture.selection.isSelected(&second) << second.isVisible()
                   << second.isMinimized() << Access::shakeContentReady(second) << second.groupId()
                   << "thumbnail" << fixture.selection.isSelected(&thumbnail) << "tab"
                   << fixture.selection.isSelected(&tab) << "clickThrough"
                   << fixture.selection.isSelected(&clickThrough) << "activeGroup"
                   << fixture.groups.activeGroupId() << "modes"
                   << second.persistenceSnapshot().lockedMode
                   << thumbnail.persistenceSnapshot().thumbnailMode << top.active()
                   << clickThrough.persistenceSnapshot().clickThroughMode;
    require(fixture.selection.selectedCount() == 2 && fixture.selection.isSelected(&first) &&
                fixture.selection.isSelected(&second) && second.persistenceSnapshot().lockedMode &&
                thumbnail.persistenceSnapshot().thumbnailMode && top.active() &&
                clickThrough.persistenceSnapshot().clickThroughMode,
            "Select All includes locked normal pins without changing special modes");
    action(first, "SelectAllAction").trigger();
    require(fixture.selection.selectedCount() == 2, "Select All is idempotent");

    action(first, "EnableLockAction").trigger();
    action(first, "EnableLockAction").trigger();
    for (auto* pin : {&first, &second, &thumbnail, &tab, &clickThrough}) {
        const auto state = pin->persistenceSnapshot();
        require(state.lockedMode && !state.thumbnailMode && !state.hideToTopMode,
                "batch lock settles incompatible modes for all displayed pins");
    }
    require(!hidden.persistenceSnapshot().lockedMode && !inactive.persistenceSnapshot().lockedMode,
            "mode commands do not touch excluded pins");
    action(first, "DisableLockAction").trigger();
    action(first, "EnableThumbnailAction").trigger();
    action(first, "EnableThumbnailAction").trigger();
    for (auto* pin : {&first, &second, &thumbnail, &tab, &clickThrough})
        require(pin->persistenceSnapshot().thumbnailMode && !pin->persistenceSnapshot().lockedMode,
                "batch thumbnail enables once, independently of selection");
    action(first, "DisableThumbnailAction").trigger();
    action(first, "EnableHideToTopAction").trigger();
    for (auto* pin : {&first, &second, &thumbnail, &tab, &clickThrough}) {
        auto& controller = Access::hideToTop(*pin);
        controller.animation().setCurrentTime(controller.animation().duration());
        require(controller.active(), "batch Hide to Top includes all displayed pins");
    }
    action(first, "EnableHideToTopAction").trigger();
    require(Access::batchTargets(first).size() == 5, "collapsed top tabs remain displayed");
    action(first, "DisableHideToTopAction").trigger();
    action(first, "DisableHideToTopAction").trigger();
    for (auto* pin : {&first, &second, &thumbnail, &tab, &clickThrough})
        require(!Access::hideToTop(*pin).active(), "batch disable restores top tabs");

    class Translator final : public QTranslator {
      public:
        QString translate(const char* context, const char* source, const char*,
                          int n) const override {
            if (QByteArray(context) != "ScreenshotPinnedWindow")
                return {};
            if (QByteArray(source) == "%n window(s) displayed")
                return QStringLiteral("Displayed translation %1").arg(n);
            return QStringLiteral("Translated ") + QString::fromUtf8(source);
        }
    } translator;
    QCoreApplication::installTranslator(&translator);
    const auto restore = qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
    QEvent language(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&first, &language);
    require(action(first, "EnableLockAction").text() ==
                    QStringLiteral("Translated Enable lock mode") &&
                action(first, "ImageStitchingAction")
                    .text()
                    .startsWith(QStringLiteral("Translated Image stitching")) &&
                action(first, "CopyFilesToClipboardAction")
                    .text()
                    .startsWith(QStringLiteral("Translated Copy files to clipboard")) &&
                action(first, "DisplayedCount").text() == QStringLiteral("Displayed translation 5"),
            "materialized batch menus and count retranslate live");
    second.hide();
    require(action(first, "DisplayedCount").text() == QStringLiteral("Displayed translation 4"),
            "the count refreshes each time the submenu opens");
}

void shortcuts() {
    PinnedSelectionFixture fixture;
    auto& first = fixture.add({40, 60});
    fixture.add({230, 160});
    const snow_shot::storage::PinToScreenShortcutSettings settings;
    const auto previous = settings.shortcuts(QStringLiteral("batch_select_all"));
    const auto previousCopy = settings.shortcuts(QStringLiteral("batch_copy_files_to_clipboard"));
    const auto previousStitching = settings.shortcuts(QStringLiteral("batch_image_stitching"));
    const auto restore = qScopeGuard([&] {
        settings.setShortcuts(QStringLiteral("batch_select_all"), previous);
        settings.setShortcuts(QStringLiteral("batch_copy_files_to_clipboard"), previousCopy);
        settings.setShortcuts(QStringLiteral("batch_image_stitching"), previousStitching);
    });
    const auto configured =
        snow_shot::shortcuts::bindingsFromPortableText({QStringLiteral("Ctrl+Alt+F12")});
    require(settings.setShortcuts(QStringLiteral("batch_select_all"), configured),
            "batch shortcut is configurable");
    require(!settings.setShortcuts(QStringLiteral("batch_save_as_file"), configured),
            "batch shortcuts participate in conflict detection");
    const QString legend =
        QStringLiteral("\t") + snow_shot::shortcuts::formatShortcutListDisplayText(configured);
    require(action(first, "SelectAllAction").text().endsWith(legend),
            "batch menu displays the configured shortcut");
    first.activateWindow();
    first.setFocus();
    waitForUi(10);
    sendShortcut(first, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    require(fixture.selection.selectedCount() == 2,
            "configured Select All invokes the batch command");
    fixture.selection.clearSelection();
    fixture.selection.toggleSelection(&first);
    require(settings.setShortcuts(
                QStringLiteral("batch_image_stitching"),
                snow_shot::shortcuts::bindingsFromPortableText({QStringLiteral("Ctrl+Alt+J")})),
            "configure the default batch stitching binding");
    QVector<ScreenshotPinnedImageSnapshot> stitched;
    QObject::connect(&fixture.selection,
                     &PinnedSelectionFixture::Controller::imageStitchingRequested, &first,
                     [&](const auto& snapshots) { stitched = snapshots; });
    sendShortcut(first, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    require(stitched.size() == 2 && fixture.selection.selectedCount() == 1,
            "Ctrl+Alt+J stitches all displayed pins independently of window selection");
    fixture.selection.clearSelection();
    const auto copyBinding =
        snow_shot::shortcuts::bindingsFromPortableText({QStringLiteral("Ctrl+Alt+F11")});
    require(settings.setShortcuts(QStringLiteral("batch_copy_files_to_clipboard"), copyBinding),
            "batch file-copy shortcut is configurable");
    const QString copyLegend =
        QStringLiteral("\t") + snow_shot::shortcuts::formatShortcutListDisplayText(copyBinding);
    require(action(first, "CopyFilesToClipboardAction").text().endsWith(copyLegend),
            "batch file-copy menu displays its configured shortcut");
    sendShortcut(first, Qt::Key_F11, Qt::ControlModifier | Qt::AltModifier);
    waitForSave();
    const auto copied = exportedClipboardUrls();
    require(copied.size() == 2, "configured copy shortcut copies all displayed image files");
    QLineEdit input(&first);
    input.show();
    input.setFocus();
    sendShortcut(input, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    sendShortcut(input, Qt::Key_F11, Qt::ControlModifier | Qt::AltModifier);
    sendShortcut(input, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    require(fixture.selection.selectedCount() == 0, "batch shortcuts preserve editable input");
    require(!Access::batchSaving() && exportedClipboardUrls() == copied,
            "editable input suppresses batch file-copy shortcuts");
    input.hide();
    first.setFocus();
    QDialog modal(&first);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.show();
    waitForUi(5);
    sendShortcut(first, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    sendShortcut(first, Qt::Key_F11, Qt::ControlModifier | Qt::AltModifier);
    sendShortcut(first, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    require(fixture.selection.selectedCount() == 0, "modal dialogs suppress batch commands");
    require(!Access::batchSaving() && exportedClipboardUrls() == copied,
            "modal dialogs suppress batch file-copy shortcuts");
    modal.close();
}

void lifecycle() {
    const snow_shot::storage::PinToScreenSettings settings;
    const bool closeConfirmation = settings.confirmBeforeClosingWindow();
    const bool destroyConfirmation = settings.confirmBeforeDestroyingWindow();
    const auto restore = qScopeGuard([&] {
        settings.setConfirmBeforeClosingWindow(closeConfirmation);
        settings.setConfirmBeforeDestroyingWindow(destroyConfirmation);
    });
    settings.setConfirmBeforeClosingWindow(true);
    {
        PinnedSelectionFixture fixture;
        auto& first = fixture.add({40, 60});
        auto& second = fixture.add({230, 160});
        action(first, "CloseAction").trigger();
        for (auto* pin : {&first, &second}) {
            auto* modal = pin->findChild<adqt::widgets::AdModal*>(
                QStringLiteral("screenshotPinnedCloseConfirmation"));
            require(modal && modal->isOpen(), "batch Close honors each pin's close confirmation");
            modal->reject();
        }
        require(fixture.closed == 0, "canceling Close preserves every pin");
        waitForUi(5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        settings.setConfirmBeforeClosingWindow(false);
        action(first, "CloseAction").trigger();
        require(fixture.closed == 2 && fixture.repository.summaries().size() == 2,
                "batch Close preserves restorable records");
    }
    for (bool confirm : {true, false}) {
        settings.setConfirmBeforeDestroyingWindow(confirm);
        PinnedSelectionFixture fixture;
        auto& first = fixture.add({40, 60});
        fixture.add({230, 160});
        fixture.add({430, 160}).hide();
        if (confirm) {
            action(first, "DestroyAction").trigger();
            auto* modal = first.findChild<adqt::widgets::AdModal*>(
                QStringLiteral("screenshotPinnedSelectionDestroyConfirmation"));
            require(modal && modal->property("pinnedSelectionCount").toInt() == 2 &&
                        modal->windowTitle() ==
                            snow_shot::presentation::PinnedWindowSelectionController::tr(
                                "Destroy displayed windows"),
                    "Destroy confirms the displayed target count once");
            modal->reject();
            waitForUi(5);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(fixture.destroyAttempts == 0, "canceling batch Destroy removes nothing");
        }
        fixture.rejectDestroy = true;
        action(first, "DestroyAction").trigger();
        if (confirm)
            first
                .findChild<adqt::widgets::AdModal*>(
                    QStringLiteral("screenshotPinnedSelectionDestroyConfirmation"))
                ->accept();
        waitForUi(5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(fixture.destroyAttempts == 1 && fixture.repository.summaries().size() == 3 &&
                    first.sourcePinAvailable(),
                "storage failure preserves all batch targets");
        fixture.rejectDestroy = false;
        action(first, "DestroyAction").trigger();
        if (confirm)
            first
                .findChild<adqt::widgets::AdModal*>(
                    QStringLiteral("screenshotPinnedSelectionDestroyConfirmation"))
                ->accept();
        waitForUi(5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(fixture.destroyAttempts == 2 && fixture.lastDestroyedIds.size() == 2 &&
                    fixture.repository.summaries().size() == 1,
                "Destroy removes only the displayed target records");
    }
}

void saving() {
    const snow_shot::storage::ScreenshotSettings settings;
    const auto oldName = settings.manualSaveFilenameFormat();
    const auto oldFormat = settings.lastManualSaveFormat();
    const auto oldDirectory = settings.lastManualSaveDirectory();
    const auto restore = qScopeGuard([&] {
        settings.setManualSaveFilenameFormat(oldName);
        settings.setLastManualSaveFormat(oldFormat);
        settings.setLastManualSaveDirectory(oldDirectory);
    });
    require(settings.setManualSaveFilenameFormat(QStringLiteral("Batch")) &&
                settings.setLastManualSaveFormat(QStringLiteral("png")),
            "set deterministic batch names");
    PinnedSelectionFixture fixture;
    auto& first = fixture.add({40, 60}, {120, 80});
    auto& second = fixture.add({230, 160}, {90, 60});
    QTemporaryDir output;
    require(output.isValid(), "batch export has a temporary destination");
    write(output.filePath(QStringLiteral("Batch_1.png")), "existing");
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs = qScopeGuard(
        [&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled); });
    for (bool keyboard : {false, true}) {
        bool folderPrompted = false;
        first.activateWindow();
        first.setFocus();
        waitForUi(5);
        QTimer::singleShot(0, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                    folderPrompted = dialog->fileMode() == QFileDialog::Directory;
                    dialog->reject();
                }
            }
        });
        if (keyboard)
            sendShortcut(first, Qt::Key_S, Qt::ControlModifier | Qt::AltModifier);
        else
            action(first, "SaveAsFileAction").trigger();
        waitForSave();
        require(
            folderPrompted && !first.property("saveDialogOpen").toBool() &&
                QDir(output.path()).entryList(QDir::Files).size() == 1,
            "menu and shortcut prompt for a folder; cancel writes nothing and releases the dialog");
    }
    Access::batchSave(first, output.path());
    require(Access::batchSaving(), "batch save holds the admission guard before yielding");
    Access::batchSave(second, output.path());
    waitForSave();
    require(read(output.filePath(QStringLiteral("Batch_1.png"))) == "existing" &&
                !QImage(output.filePath(QStringLiteral("Batch_2.png"))).isNull() &&
                !QImage(output.filePath(QStringLiteral("Batch_3.png"))).isNull() &&
                QDir(output.path()).entryList(QDir::Files).size() == 3 &&
                settings.lastManualSaveDirectory() == output.path(),
            "batch export skips occupied suffixes, preserves files, and rejects concurrent saves");
    require(QImage(output.filePath(QStringLiteral("Batch_2.png"))).size() !=
                QImage(output.filePath(QStringLiteral("Batch_3.png"))).size(),
            "each pin exports its own image in registration order");

    QTemporaryDir partial;
    Access::batchSave(first, partial.path());
    Access::changeBatchIdentity(second);
    waitForSave();
    require(QDir(partial.path()).entryList(QDir::Files).size() == 1,
            "a reused target is skipped without preventing other exports");
    QTemporaryDir closed;
    Access::batchSave(first, closed.path());
    second.close();
    waitForSave();
    require(QDir(closed.path()).entryList(QDir::Files).size() == 1,
            "a closed target is skipped and the remaining snapshot completes");
    QTemporaryDir jpeg;
    require(settings.setLastManualSaveFormat(QStringLiteral("jpeg")),
            "configure a remembered manual image format");
    Access::batchSave(first, jpeg.path());
    waitForSave();
    const auto jpegFiles = QDir(jpeg.path()).entryList(QDir::Files);
    require(jpegFiles.size() == 1 && read(QDir(jpeg.path()).filePath(jpegFiles.first()))
                                         .startsWith(QByteArray::fromHex("ffd8")),
            "batch image export honors the remembered format");
    Access::batchSave(first, output.filePath(QStringLiteral("Batch_1.png")));
    waitForSave();
    require(read(output.filePath(QStringLiteral("Batch_1.png"))) == "existing",
            "an invalid destination reports failure without modifying existing data");

    QTemporaryDir abandoned;
    auto owner = std::make_unique<ScreenshotPinnedWindow>();
    Access::restoreOffscreen(*owner, cachedOcrPinConfig(nullptr));
    owner->show();
    Access::batchSave(*owner, abandoned.path());
    owner.reset();
    waitForSave();
    require(QDir(abandoned.path()).entryList(QDir::Files).isEmpty(),
            "destroying the initiating window cancels the pending batch");
}

void copying() {
    const snow_shot::storage::ScreenshotSettings settings;
    const auto oldName = settings.autoSaveFilenameFormat();
    const auto oldFormat = settings.imageFormat();
    const auto oldDirectory = settings.lastManualSaveDirectory();
    const auto oldCopyFiles = settings.copyImageFileToClipboard();
    const auto restore = qScopeGuard([&] {
        settings.setAutoSaveFilenameFormat(oldName);
        settings.setImageFormat(oldFormat);
        settings.setCopyImageFileToClipboard(oldCopyFiles);
    });
    require(settings.setAutoSaveFilenameFormat(QStringLiteral("BatchCopy")) &&
                settings.setImageFormat(QStringLiteral("png")) &&
                settings.setCopyImageFileToClipboard(false),
            "explicit batch file copying uses copy output settings independently of copy mode");
    PinnedSelectionFixture fixture;
    auto& first = fixture.add({40, 60}, {120, 80});
    auto& second = fixture.add({230, 160}, {90, 70});
    auto& hidden = fixture.add({420, 160});
    hidden.hide();
    fixture.selection.toggleSelection(&second);
    auto* clipboard = QApplication::clipboard();
    clipboard->setText(QStringLiteral("before batch"));
    action(first, "CopyFilesToClipboardAction").trigger();
    require(Access::batchSaving() && !action(second, "SaveAsFileAction").isEnabled() &&
                !action(second, "CopyFilesToClipboardAction").isEnabled(),
            "copy and save share one admission guard across all pins");
    QTemporaryDir blocked;
    Access::batchSave(second, blocked.path());
    waitForSave();
    const auto urls = clipboard->mimeData()->urls();
    require(urls.size() == 2 && QDir(blocked.path()).entryList(QDir::Files).isEmpty(),
            "displayed copy ignores selection, excludes hidden pins, and blocks another batch");
    const QString firstPath = urls.at(0).toLocalFile();
    const QString secondPath = urls.at(1).toLocalFile();
    require(QFileInfo(firstPath).fileName() == QStringLiteral("BatchCopy_1.png") &&
                QFileInfo(secondPath).fileName() == QStringLiteral("BatchCopy_2.png") &&
                QFileInfo(firstPath).absolutePath() == QFileInfo(secondPath).absolutePath() &&
                QImage(firstPath).size() == QSize(120, 80) &&
                QImage(secondPath).size() == QSize(90, 70) &&
                QImage(firstPath).pixelColor(60, 40) == QColor(42, 84, 126) &&
                settings.lastManualSaveDirectory() == oldDirectory,
            "batch file URLs preserve order, rendered pixels, unique names and manual settings");
    clipboard->setText(QStringLiteral("replacement"));
    require(QFileInfo::exists(firstPath) && QFileInfo::exists(secondPath),
            "completed batch files outlive the session and clipboard replacement");

    require(settings.setImageFormat(QStringLiteral("bmp")) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("Frozen")),
            "configure batch clipboard export options");
    Access::batchCopy(first, {&second, &first});
    settings.setImageFormat(QStringLiteral("png"));
    settings.setAutoSaveFilenameFormat(QStringLiteral("Later"));
    waitForSave();
    const auto frozen = clipboard->mimeData()->urls();
    require(frozen.size() == 2 &&
                QFileInfo(frozen.at(0).toLocalFile()).fileName() ==
                    QStringLiteral("Frozen_1.bmp") &&
                read(frozen.at(0).toLocalFile()).startsWith("BM") &&
                QImage(frozen.at(0).toLocalFile()).size() == QSize(90, 70),
            "copy freezes settings at invocation and honors explicit target order and format");

    Access::batchCopy(first, {&second, &first});
    Access::changeBatchIdentity(second);
    waitForSave();
    const auto partial = clipboard->mimeData()->urls();
    require(partial.size() == 1 && QImage(partial.first().toLocalFile()).size() == QSize(120, 80),
            "a recycled target fails while successfully exported files still reach the clipboard");

    clipboard->setText(QStringLiteral("all failed"));
    Access::batchCopy(first, {&second});
    Access::changeBatchIdentity(second);
    waitForSave();
    require(clipboard->text() == QStringLiteral("all failed"),
            "an entirely failed batch leaves the clipboard unchanged");
    require(settings.setAutoSaveFilenameFormat(emptyExportFilenameFormat()),
            "configure an invalid clipboard filename");
    Access::batchCopy(first, {&first});
    waitForSave();
    require(clipboard->text() == QStringLiteral("all failed"),
            "invalid filenames never publish empty or incomplete clipboard files");
    settings.setAutoSaveFilenameFormat(QStringLiteral("Cancelled"));
    Access::batchCopy(first, {&first});
    Access::changeBatchIdentity(first);
    waitForSave();
    require(clipboard->text() == QStringLiteral("all failed"),
            "recycling the initiating window cancels the batch without clipboard publication");

    settings.setImageFormat(QStringLiteral("pdf"));
    Access::batchCopy(first, {&first});
    waitForSave();
    const auto pdf = clipboard->mimeData()->urls();
    require(pdf.size() == 1 && read(pdf.first().toLocalFile()).startsWith("%PDF-"),
            "batch file copying supports the existing clipboard PDF format");
    clipboard->setText(QStringLiteral("newer publication"));
    Access::batchCopy(first, {&first});
    static_cast<void>(ScreenshotClipboardService::reservePublication());
    waitForSave();
    require(clipboard->text() == QStringLiteral("newer publication"),
            "an older pending batch cannot replace a newer clipboard publication");

    {
        auto owner = std::make_unique<ScreenshotPinnedWindow>();
        Access::restoreOffscreen(*owner, cachedOcrPinConfig(nullptr));
        owner->show();
        Access::batchCopy(*owner, {owner.get()});
    }
    waitForSave();
    require(clipboard->text() == QStringLiteral("newer publication"),
            "destroying the initiating window cancels pending file exports");
}

void newerBatchCopyCancelsSingleCopy() {
    const snow_shot::storage::ScreenshotSettings settings;
    const auto oldActions = settings.autoSaveAfterExecutionActions();
    const auto oldName = settings.autoSaveFilenameFormat();
    const auto oldFormat = settings.imageFormat();
    const auto oldCopyFiles = settings.copyImageFileToClipboard();
    const auto restore = qScopeGuard([&] {
        settings.setAutoSaveAfterExecutionActions(oldActions);
        settings.setAutoSaveFilenameFormat(oldName);
        settings.setImageFormat(oldFormat);
        settings.setCopyImageFileToClipboard(oldCopyFiles);
    });
    require(settings.setAutoSaveAfterExecutionActions({}) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("SupersedingBatch")) &&
                settings.setImageFormat(QStringLiteral("png")) &&
                settings.setCopyImageFileToClipboard(true),
            "configure single-copy cancellation without automatic saving");
    const auto wait = [](auto predicate, const char* message) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 10000)
            waitForUi(5);
        require(predicate(), message);
    };
    PinnedSelectionFixture fixture;
    auto& owner = fixture.add({40, 60}, {120, 80});
    auto& target = fixture.add({230, 160}, {90, 70});
    Access::copyCurrentViewport(owner);
    require(Access::exportArtifact(owner) != nullptr,
            "the earlier single-image copy is preparing an export artifact");
    Access::batchCopy(owner, {&target});
    require(Access::exportArtifact(owner) == nullptr,
            "starting a batch cancels the initiating pin's earlier export artifact");
    waitForSave();
    const auto prepared = exportedClipboardUrls();
    require(prepared.size() == 1 && QImage(prepared.first().toLocalFile()).size() == QSize(90, 70),
            "the batch target replaces an already-preparing single-image copy");

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
    const auto closeLazy = qScopeGuard([&] { lazy.close(); });
    require(lazy.present(config), "present a pin whose initial image is still loading");
    wait([&] { return static_cast<bool>(deliver); }, "the delayed image loader starts");
    require(Access::originalImage(lazy).isNull(),
            "the single-image copy starts before the first content frame");
    Access::copyCurrentViewport(lazy);
    Access::batchCopy(lazy, {&target});
    waitForSave();
    const auto newer = exportedClipboardUrls();
    require(newer.size() == 1 && QImage(newer.first().toLocalFile()).size() == QSize(90, 70),
            "the new batch copies its target while the earlier single-image snapshot is pending");
    bool snapshotReady = false;
    lazy.requestRenderedImageSnapshot([&](ScreenshotPinnedImageSnapshot snapshot) {
        require(snapshot.source.isValid() && snapshot.error.isEmpty(),
                "the delayed image can still produce a rendered snapshot");
        snapshotReady = true;
    });
    deliver(config.imageSource.materializedImage);
    wait(
        [&] {
            return snapshotReady && !Access::exportArtifact(lazy) &&
                   ScreenshotExportCoordinator::shared().pendingJobCount() == 0;
        },
        "materialization and any superseded single-copy work finish");
    require(exportedClipboardUrls() == newer,
            "an older lazy single-image copy cannot overwrite the completed batch clipboard");
}

void recognitionSaving() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    const snow_shot::storage::ScreenshotSettings settings;
    const auto oldName = settings.manualSaveFilenameFormat();
    const auto oldFormat = settings.lastManualSaveFormat();
    const auto oldDirectory = settings.lastManualSaveDirectory();
    const snow_shot::storage::ScreenshotImageConversionSettings conversionSettings;
    const auto oldModel = conversionSettings.visionModel();
    const auto restore = qScopeGuard([&] {
        settings.setManualSaveFilenameFormat(oldName);
        settings.setLastManualSaveFormat(oldFormat);
        settings.setLastManualSaveDirectory(oldDirectory);
        conversionSettings.setVisionModel(oldModel);
    });
    settings.setManualSaveFilenameFormat(QStringLiteral("Mixed"));
    settings.setLastManualSaveFormat(QStringLiteral("png"));
    conversionSettings.setVisionModel(QStringLiteral("vision-saved"));
    PinnedSelectionFixture fixture;
    auto& image = fixture.add({40, 60});
    auto config = cachedOcrPinConfig(nullptr);
    const QString source = QStringLiteral("# Batch document\n\n**Exact source**");
    config.recognitionResults.conversions = {
        {SnowShotImageConversionFormat::Markdown, QStringLiteral("vision-saved"), source}};
    config.recognitionResults.visibleConversion = SnowShotImageConversionFormat::Markdown;
    ScreenshotPinnedWindow markdown;
    markdown.setAttribute(Qt::WA_DeleteOnClose, false);
    require(markdown.present(config), "present a mixed-format batch recognition pin");
    waitForUi(100);
    auto* session = markdown.findChild<ScreenshotRecognitionSessionController*>();
    require(session != nullptr, "recognition pin has a session");
    session->activate(ScreenshotRecognitionSessionController::Mode::Markdown);
    require(session->fileExportSnapshot().has_value(), "Markdown keeps its native file export");
    QTemporaryDir output;
    write(output.filePath(QStringLiteral("Mixed_2.txt")), "existing companion");
    Access::batchSave(image, output.path());
    waitForSave();
    require(
        !QImage(output.filePath(QStringLiteral("Mixed_1.png"))).isNull() &&
            read(output.filePath(QStringLiteral("Mixed_2.txt"))) == "existing companion" &&
            !QFileInfo::exists(output.filePath(QStringLiteral("Mixed_2.md"))) &&
            read(output.filePath(QStringLiteral("Mixed_3.md"))) == source.toUtf8() &&
            read(output.filePath(QStringLiteral("Mixed_3.txt"))) == source.toUtf8(),
        "mixed exports preserve recognition formats and skip companion-file collisions together");
#endif
}
} // namespace pinned_batch_test

void pinnedBatchActionsTests() {
    pinned_batch_test::menuSelectionAndModes();
    pinned_batch_test::shortcuts();
    pinned_batch_test::lifecycle();
    pinned_batch_test::saving();
    pinned_batch_test::copying();
    pinned_batch_test::newerBatchCopyCancelsSingleCopy();
    pinned_batch_test::recognitionSaving();
}
