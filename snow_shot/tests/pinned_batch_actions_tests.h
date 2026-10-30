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
    select.trigger();
    require(fixture.selection.selectedCount() == 2 && fixture.selection.isSelected(&first) &&
                fixture.selection.isSelected(&second) && second.persistenceSnapshot().lockedMode &&
                thumbnail.persistenceSnapshot().thumbnailMode && top.active() &&
                clickThrough.persistenceSnapshot().clickThroughMode,
            "Select All includes locked normal pins without changing special modes");
    select.trigger();
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
                    QStringLiteral("Translated Enable Lock Mode") &&
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
    const auto restore =
        qScopeGuard([&] { settings.setShortcuts(QStringLiteral("batch_select_all"), previous); });
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
    QLineEdit input(&first);
    input.show();
    input.setFocus();
    sendShortcut(input, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    require(fixture.selection.selectedCount() == 0, "batch shortcuts preserve editable input");
    input.hide();
    first.setFocus();
    QDialog modal(&first);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.show();
    waitForUi(5);
    sendShortcut(first, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    require(fixture.selection.selectedCount() == 0, "modal dialogs suppress batch commands");
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
    pinned_batch_test::recognitionSaving();
}
