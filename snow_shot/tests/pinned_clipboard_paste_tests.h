// SPDX-License-Identifier: GPL-3.0-or-later
// Included by screenshot_pinned_window_tests.cpp after its shared test helpers.

void pinnedClipboardPasteTests() {
    using Access = ScreenshotPinnedWindowTestAccess;
    auto* clipboard = QApplication::clipboard();
    const auto clearClipboard = qScopeGuard([&] { clipboard->clear(); });
    QImage original(320, 200, QImage::Format_ARGB32_Premultiplied);
    original.setColorSpace(QColorSpace::SRgb);
    original.fill(QColor(20, 40, 60));
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = QRect(40, 60, 320, 200);
    config.canvasSourceRect = QRectF(0, 0, 320, 200);
    config.initialWindowSize = original.size();
    config.imageSource = ScreenshotImageSource::fromImage(original, config.canvasSourceRect);
    ScreenshotPinnedWindow window;
    ScreenshotPinnedWindow peer;
    Access::prepareReplacement(window, config);
    Access::prepareReplacement(peer, config);
    peer.show();
    window.show();
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    require(canvas != nullptr, "Paste fixture owns a canvas");

    const auto focus = [](QWidget& target) {
        target.window()->activateWindow();
        target.setFocus();
        waitForUi(20);
        require(target.hasFocus(), "Paste target owns keyboard focus");
    };
    const auto paste = [](QWidget& target, QKeyCombination combination, bool repeat = false) {
        PhysicalKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(),
                               {}, repeat);
        QCoreApplication::sendEvent(&target, &press);
        PhysicalKeyEvent release(QEvent::KeyRelease, combination.key(),
                                 combination.keyboardModifiers());
        QCoreApplication::sendEvent(&target, &release);
    };
    const auto waitForReplacement = [](ScreenshotPinnedWindow& target) {
        live_pinned_tests::wait([&] { return !Access::replacementPending(target); },
                                "clipboard replacement finishes");
    };
    const auto samePixels = [](const QImage& first, const QImage& second) {
        return first.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
               second.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };
    QList<QKeyCombination> combinations;
    for (const auto& sequence : QKeySequence::keyBindings(QKeySequence::Paste)) {
        if (sequence.count() == 1 && !combinations.contains(sequence[0]))
            combinations.append(sequence[0]);
    }
    require(!combinations.isEmpty(), "platform supplies standard Paste bindings");
    const auto combination = combinations.first();
    QImage replacement = original;
    int variant = 0;
    for (const auto binding : combinations) {
        replacement.fill(QColor(80 + variant, 100, 120));
        QWidget& target = variant++ % 2 == 0 ? static_cast<QWidget&>(window) : *canvas;
        focus(target);
        clipboard->setImage(replacement);
        paste(target, binding);
        require(Access::replacementPending(window), "standard Paste starts asynchronous loading");
        waitForReplacement(window);
        require(samePixels(Access::originalImage(window), replacement) &&
                    samePixels(Access::originalImage(peer), original),
                "each standard Paste binding replaces only the receiving pin");
    }

    focus(*canvas);
    clipboard->setImage(original);
    paste(*canvas, combination, true);
    require(!Access::replacementPending(window), "automatic key repeats do not reload content");
    Access::selectionFramePublished(window, false);
    paste(*canvas, combination);
    require(!Access::replacementPending(window), "unpublished pins do not handle Paste");
    Access::selectionFramePublished(window, true);
    window.setProperty("saveDialogOpen", true);
    paste(*canvas, combination);
    require(!Access::replacementPending(window), "save dialogs block content Paste");
    window.setProperty("saveDialogOpen", false);
    {
        QDialog dialog(&window);
        dialog.setWindowModality(Qt::WindowModal);
        dialog.show();
        waitForUi(20);
        require(QApplication::activeModalWidget() == &dialog, "Paste modal fixture is active");
        paste(*canvas, combination);
        require(!Access::replacementPending(window), "modal dialogs block content Paste");
        dialog.reject();
    }
    {
        QWidget unrelated;
        unrelated.show();
        focus(unrelated);
        paste(unrelated, combination);
        require(!Access::replacementPending(window) && !Access::replacementPending(peer),
                "unrelated windows do not dispatch pinned Paste");
    }
    {
        QLineEdit input(&window);
        input.show();
        focus(input);
        clipboard->setText(QStringLiteral("Editable paste"));
        paste(input, combination);
        require(input.text() == QStringLiteral("Editable paste") &&
                    !Access::replacementPending(window),
                "editable controls retain normal text Paste");
    }

    Access::editSelectionOffscreen(window, true);
    auto* controller = window.findChild<ScreenshotPinnedEditController*>();
    auto* palette = controller && controller->toolbarWindow()
                        ? controller->toolbarWindow()->palette()
                        : nullptr;
    require(palette && palette->activateDrawingShortcut(QStringLiteral("text")),
            "annotation Paste fixture activates the text tool");
    focus(*canvas);
    const QPointF anchor = canvas->canvasToViewTransform().map(QPointF(80, 80));
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonPress, anchor, Qt::LeftButton,
                                       Qt::LeftButton);
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonRelease, anchor, Qt::LeftButton,
                                       Qt::NoButton);
    require(canvas->hasActiveTextEditing(), "annotation text editor is active");
    clipboard->setText(QStringLiteral("Pasted annotation"));
    paste(*canvas, combination);
    require(canvas->hasActiveTextEditing() && !Access::replacementPending(window) &&
                samePixels(Access::originalImage(window), replacement),
            "active canvas text editing keeps Paste in the text editor");
    require(canvas->resetEditingStatePreservingTool(), "commit the pasted annotation");
    const auto document = Access::dragDocument(window);
    const auto history = Access::drawingHistory(window);
    require(document.contains("Pasted annotation"), "canvas text editor receives pasted text");
    Access::setGeneralOpacity(window, 70);
    const auto before = window.persistenceSnapshot();
    clipboard->setImage(original);
    paste(*canvas, combination);
    require(Access::replacementPending(window), "Paste loads content in annotation mode");
    waitForReplacement(window);
    const auto after = window.persistenceSnapshot();
    require(samePixels(Access::originalImage(window), original) && controller->editMode() &&
                Access::dragDocument(window) == document &&
                Access::drawingHistory(window) == history &&
                before.nativeGeometry == after.nativeGeometry &&
                before.scalePercent == after.scalePercent &&
                before.opacityPercent == after.opacityPercent &&
                before.imageTransform == after.imageTransform,
            "annotation-mode Paste preserves drawing history and window settings");

    // Configurable shortcut reloads must leave the fixed standard Paste binding intact.
    const snow_shot::storage::PinToScreenShortcutSettings shortcuts;
    const auto previous = shortcuts.shortcuts(QStringLiteral("copy_original_content"));
    const auto restoreShortcuts = qScopeGuard([&] {
        require(shortcuts.setShortcuts(QStringLiteral("copy_original_content"), previous),
                "restore copy-original shortcuts");
    });
    require(shortcuts.setShortcuts(QStringLiteral("copy_original_content"), {}),
            "reload configurable pinned shortcuts");
    clipboard->setText(QStringLiteral("Replacement content"));
    paste(*canvas, combination);
    require(Access::replacementPending(window), "Paste survives configurable shortcut reloads");
    waitForReplacement(window);
    require(window.persistenceSnapshot().originalText == QStringLiteral("Replacement content") &&
                Access::dragDocument(window) == document &&
                Access::drawingHistory(window) == history,
            "text Paste replaces source content while retaining annotations");
    const QImage textImage = Access::originalImage(window);
    for (bool unsupported : {false, true}) {
        auto* data = new QMimeData;
        if (unsupported)
            data->setData(QStringLiteral("application/x-unsupported-test"), QByteArray("value"));
        clipboard->setMimeData(data);
        paste(*canvas, combination);
        waitForReplacement(window);
        require(samePixels(Access::originalImage(window), textImage) &&
                    Access::dragDocument(window) == document,
                "empty and unsupported clipboard content preserve the pin");
    }

    auto source = std::make_shared<live_pinned_tests::FakeSource>();
    ScreenshotPinnedWindow live;
    live_pinned_tests::present(live, live_pinned_tests::config(source));
    require(live.isLiveWindow() && Access::shakeContentReady(live) &&
                !Access::originalImage(live).isNull(),
            "live Paste fixture has published content");
    focus(live);
    clipboard->setImage(original);
    paste(live, combination);
    require(live.isLiveWindow() && !Access::replacementPending(live),
            "live windows do not load clipboard content on Paste");
}
