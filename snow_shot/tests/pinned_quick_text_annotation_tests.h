#ifndef SNOW_SHOT_TESTS_PINNED_QUICK_TEXT_ANNOTATION_TESTS_H
#define SNOW_SHOT_TESTS_PINNED_QUICK_TEXT_ANNOTATION_TESTS_H

#include "widgets/popover.h"

namespace pinned_quick_text_annotation_tests {
using Access = ScreenshotPinnedWindowTestAccess;
using Tool = ScreenshotTextAnnotationTool;

struct SettingsGuard final {
    snow_shot::storage::PinToScreenSettings settings;
    QString selection = settings.textSelectionOnRecognitionResults();
    bool quick = settings.quickAnnotationForSelectedText();
    SettingsGuard() {
        require(settings.setTextSelectionOnRecognitionResults(QStringLiteral("always")) &&
                    settings.setQuickAnnotationForSelectedText(true),
                "enable quick annotation on canvas text");
    }
    ~SettingsGuard() {
        static_cast<void>(settings.setTextSelectionOnRecognitionResults(selection));
        static_cast<void>(settings.setQuickAnnotationForSelectedText(quick));
    }
};

struct Fixture final {
    ScreenshotPinnedWindow::Config config;
    ScreenshotPinnedWindow window;
    ScreenshotRecognitionSessionController* session = nullptr;
    ScreenshotRecognitionWindow* content = nullptr;
    QGraphicsView* formattedView = nullptr;
    QGraphicsTextItem* textItem = nullptr;

    explicit Fixture(int kind = 0, qreal dpr = 1.0, bool masked = false)
        : config(cachedOcrPinConfig(nullptr)) {
        if (kind == 0) {
            auto& presentation = *config.recognitionResults.text->presentation;
            auto line = presentation.lines.first();
            line.text = QStringLiteral("Second unstyled passage");
            for (auto& point : line.quad)
                point.ry() += 50;
            presentation.lines.push_back(line);
            presentation.prepareForRendering();
        } else {
            QMimeData mime;
            if (kind == 2)
                mime.setHtml(QStringLiteral("<p><b>Selectable</b> pinned HTML</p>"
                                            "<p>Second unstyled passage</p>"));
            else
                mime.setText(QStringLiteral("Selectable pinned text\nSecond unstyled passage"));
            auto snapshot =
                ScreenshotClipboardContentReader::snapshotMimeData(&mime, dpr, Qt::white);
            require(snapshot.has_value(), "snapshot quick-annotation formatted fixture");
            auto decoded = decodeScreenshotDropContent(std::move(*snapshot));
            require(decoded && decoded->isFormattedText(), "decode quick-annotation formatted pin");
            config.recognitionResults = {};
            config.canvasSourceRect = QRectF(QPointF(), QSizeF(decoded->image.size()));
            config.contentCanvasRect = config.canvasSourceRect;
            config.surfaceCanvasRect = config.canvasSourceRect;
            config.imageSource =
                ScreenshotImageSource::fromImage(decoded->image, config.canvasSourceRect);
            config.initialWindowSize = decoded->image.size();
            config.nativeGeometry =
                physicalPinGeometry(*config.screen, QPoint(100, 180), config.initialWindowSize);
            config.formattedTextDocument = decoded->formattedDocument;
            config.formattedPlainText = decoded->plainText;
            config.formattedTextDevicePixelRatio = decoded->formattedTextDevicePixelRatio;
            config.originalClipboardContent = decoded->originalContent;
        }
        if (masked) {
            const auto size = config.canvasSourceRect.size().toSize();
            config.resultStyle.cornerRadius = 12;
            config.resultStyle.region =
                QRegion(QRect(QPoint(), size))
                    .subtracted(QRect(0, 0, size.width() / 2, size.height()));
        }
        session = Access::hiddenSelectionOffscreen(window, config);
        content = window.findChild<ScreenshotRecognitionWindow*>(
            QStringLiteral("screenshotPinnedRecognitionContent"));
        require(session && content && Access::hiddenSelection(window),
                "quick-annotation fixture owns hidden canvas text selection");
        Access::finishInitialTextDisplay(window, false);
        formattedView = window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
        if (formattedView) {
            for (auto* item : formattedView->scene()->items())
                if (auto* candidate = dynamic_cast<QGraphicsTextItem*>(item))
                    textItem = candidate;
            require(textItem != nullptr, "formatted fixture has its original text document");
        }
        window.activateWindow();
        keyTarget()->setFocus();
        QApplication::processEvents();
    }

    QWidget* inputTarget() const {
        return formattedView ? formattedView->viewport() : static_cast<QWidget*>(content);
    }
    QWidget* keyTarget() const {
        return formattedView ? static_cast<QWidget*>(formattedView) : content;
    }
    QPoint point(int character, int row = 0) const {
        if (textItem) {
            auto block = textItem->document()->firstBlock();
            for (int i = 0; i < row; ++i)
                block = block.next();
            const auto line = block.layout()->lineAt(0);
            return formattedView->mapFromScene(textItem->mapToScene(
                block.layout()->position() +
                QPointF(line.cursorToX(character), line.y() + line.height() / 2)));
        }
        const auto& line = Access::displayedRecognition(window).lines.at(row);
        const auto rect = line.quad.boundingRect();
        const qreal fraction = static_cast<qreal>(character) / static_cast<qreal>(line.text.size());
        const QPointF source(rect.left() + rect.width() * fraction, rect.center().y());
        auto* canvas = window.findChild<SnowCanvasWidget*>();
        return content->mapFromGlobal(
            canvas->mapToGlobal(canvas->canvasToViewTransform().map(source).toPoint()));
    }
    void pointer(QEvent::Type type, QPoint point, Qt::MouseButton changed,
                 Qt::MouseButtons held) const {
        auto* target = inputTarget();
        QMouseEvent event(type, QPointF(point), QPointF(target->mapToGlobal(point)), changed, held,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    }
    void drag(int start = 1, int end = 7, int row = 0, bool release = true) {
        pointer(QEvent::MouseButtonPress, point(start, row), Qt::LeftButton, Qt::LeftButton);
        pointer(QEvent::MouseMove, point(end, row), Qt::NoButton, Qt::LeftButton);
        if (release)
            pointer(QEvent::MouseButtonRelease, point(end, row), Qt::LeftButton, Qt::NoButton);
    }
    void selectAll() {
        keyTarget()->setFocus();
        sendShortcut(*keyTarget(), Qt::Key_A, Qt::ControlModifier);
    }
    ScreenshotTextSelectionSnapshot selection() const {
        return content->textSelectionSnapshot();
    }
    ScreenshotQuickTextAnnotationToolbar* toolbar() {
        return Access::quickTextToolbar(window);
    }
    bool toolbarVisible() {
        auto* value = toolbar();
        return value && value->isVisible();
    }
    adqt::widgets::AdButton* button(Tool tool) {
        const QString names[] = {QStringLiteral("highlight"), QStringLiteral("wavy_underline"),
                                 QStringLiteral("underline"), QStringLiteral("strikethrough")};
        require(toolbar() != nullptr, "selection materializes the quick annotation toolbar");
        for (auto* popover : toolbar()->findChildren<adqt::widgets::AdPopover*>()) {
            auto* root = popover->contentWidget();
            if (root && root->objectName() == QStringLiteral("quickTextAnnotationToolbar")) {
                auto* value = root->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("quickTextAnnotation_%1").arg(names[static_cast<int>(tool)]));
                require(value != nullptr, "quick annotation button exists on the visible popover");
                return value;
            }
        }
        require(false, "quick annotation main popover exists");
        return nullptr;
    }
    void click(Tool tool) {
        button(tool)->click();
    }
    bool has(Tool tool, bool full = true) const {
        return Access::quickTextAnnotations(window).hasStyle(selection().ranges, tool, full);
    }
};

QImage widgetBackground(
    ScreenshotPinnedWindow& window,
    SnowCanvasRenderContext::Purpose purpose = SnowCanvasRenderContext::Purpose::Widget) {
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    QImage image(canvas->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    SnowCanvasRenderContext context{image.rect(), QRegion(image.rect()),
                                    canvas->canvasToViewTransform(), 1.0, purpose};
    Access::renderer(window).renderBeforeCanvas(painter, context);
    return image;
}

QImage exportedImage(ScreenshotPinnedWindow& window) {
    auto artifact = Access::fileSave(window);
    bool finished = false;
    ScreenshotExportImageResult result;
    require(artifact && artifact->requestImage(&window,
                                               [&](ScreenshotExportImageResult value) {
                                                   result = std::move(value);
                                                   finished = true;
                                               }),
            "selected-text annotation export starts");
    translation_tests::waitUntil([&] { return finished; },
                                 "selected-text annotation export finishes");
    require(result.succeeded(), "selected-text annotation image export succeeds");
    return result.image;
}

void onDemandToolbarLifetime() {
    SettingsGuard settings;
    for (int kind : {0, 1, 2}) {
        Fixture fixture(kind);
        require(fixture.toolbar() == nullptr,
                "pins do not create quick annotation controls before a completed selection");
        fixture.drag(1, 7, 0, false);
        require(fixture.toolbar() == nullptr,
                "a held selection gesture does not create quick annotation controls");
        fixture.pointer(QEvent::MouseButtonRelease, fixture.point(7), Qt::LeftButton, Qt::NoButton);
        require(fixture.toolbarVisible(), "a completed selection creates its quick toolbar");
        fixture.click(Tool::Underline);
        QPointer<ScreenshotQuickTextAnnotationToolbar> retired = fixture.toolbar();
        QList<QPointer<QWidget>> retiredWidgets;
        retiredWidgets.append(fixture.window.findChild<QWidget*>(
            QStringLiteral("quickTextAnnotationSelectionAnchor")));
        for (auto* popover : retired->findChildren<adqt::widgets::AdPopover*>()) {
            retiredWidgets.append(popover->contentWidget());
            retiredWidgets.append(popover->surfaceWidget());
            if (auto* content = popover->contentWidget()) {
                for (auto* picker : content->findChildren<adqt::widgets::AdColorPicker*>()) {
                    picker->setPopupVisible(true);
                    retiredWidgets.append(picker->popupContent());
                    for (auto* nested : picker->findChildren<adqt::widgets::AdPopover*>())
                        retiredWidgets.append(nested->surfaceWidget());
                    picker->setPopupVisible(false);
                }
            }
        }
        const auto annotations = Access::quickTextAnnotations(fixture.window).serialize();
        fixture.content->clearTextSelection();
        require(fixture.toolbar() == nullptr && retired && !retired->isVisible(),
                "clearing selection immediately detaches and hides its toolbar");
        for (int pass = 0; pass < 3; ++pass)
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!retired, "clearing selection destroys the toolbar after its callbacks return");
        for (const auto& widget : retiredWidgets)
            require(!widget, "retiring the toolbar releases its anchor, content and popup widgets");
        fixture.drag();
        require(fixture.toolbarVisible() && fixture.has(Tool::Underline) &&
                    Access::quickTextAnnotations(fixture.window).serialize() == annotations,
                "reselecting recreates controls and restores applied styles without changing ink");
        QPointer<ScreenshotQuickTextAnnotationToolbar> hidden = fixture.toolbar();
        fixture.window.hide();
        require(fixture.toolbar() == nullptr && hidden && !hidden->isVisible(),
                "hiding the pin retires its toolbar immediately");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!hidden, "a hidden pin releases its quick toolbar");
    }
}

void immediateSelectionAndStyleRules() {
    SettingsGuard settings;
    for (int kind : {0, 1, 2}) {
        Fixture fixture(kind, kind == 2 ? 2.0 : 1.0);
        const QImage pristine = widgetBackground(fixture.window);
        fixture.drag(1, 7, 0, false);
        require(!fixture.selection().empty() && fixture.selection().inProgress &&
                    !fixture.toolbarVisible() && fixture.content->textSelectionActive(),
                "OCR, text and HTML drag selections defer quick tools while the mouse is held");
        fixture.pointer(QEvent::MouseButtonRelease, fixture.point(7), Qt::LeftButton, Qt::NoButton);
        require(fixture.toolbarVisible() && !fixture.selection().inProgress,
                "completing a drag shows quick tools even when its final range is unchanged");
        auto originalMime = fixture.content->textSelectionClipboardMimeData();
        require(originalMime && !originalMime->text().isEmpty(),
                "selected text has clipboard data");
        fixture.click(Tool::Highlight);
        require(fixture.has(Tool::Highlight) && fixture.toolbarVisible(),
                "highlight applies immediately and keeps the popover open");
        fixture.click(Tool::Underline);
        require(fixture.has(Tool::Underline) && fixture.has(Tool::Highlight),
                "highlight coexists with underline");
        fixture.click(Tool::WavyUnderline);
        require(fixture.has(Tool::WavyUnderline) && !fixture.has(Tool::Underline, false) &&
                    fixture.has(Tool::Highlight),
                "wavy underline replaces the other line type and preserves highlight");
        fixture.click(Tool::Strikethrough);
        require(fixture.has(Tool::Strikethrough) && !fixture.has(Tool::WavyUnderline, false) &&
                    !fixture.has(Tool::Underline, false),
                "strikethrough replaces both underline variants");
        fixture.click(Tool::Strikethrough);
        require(!fixture.has(Tool::Strikethrough, false) && fixture.has(Tool::Highlight),
                "clicking an active tool removes only that style");
        const auto styledMime = fixture.content->textSelectionClipboardMimeData();
        require(styledMime && styledMime->text() == originalMime->text() &&
                    styledMime->html() == originalMime->html(),
                "quick annotation does not alter selected plain text or rich clipboard formatting");
        const auto annotated = Access::quickTextAnnotations(fixture.window).serialize();
        require(widgetBackground(fixture.window) != pristine &&
                    !Access::quickTextSnapshot(fixture.window).empty(),
                "selected-text styles appear in the canvas background layer");
        fixture.drag(1, 7, 0, false);
        require(fixture.selection().inProgress && !fixture.toolbarVisible() &&
                    Access::quickTextAnnotations(fixture.window).serialize() == annotated,
                "dragging an already selected styled passage hides its toolbar without restyling");
        fixture.pointer(QEvent::MouseButtonRelease, fixture.point(7), Qt::LeftButton, Qt::NoButton);
        const auto completedSelection = fixture.selection();
        // Native formatted text can treat a press inside its existing selection as a drag
        // candidate and collapse the cursor on release. Visibility follows the resulting range.
        require(!completedSelection.inProgress &&
                    fixture.toolbarVisible() == !completedSelection.empty() &&
                    Access::quickTextAnnotations(fixture.window).serialize() == annotated,
                "releasing a styled passage follows its native selection without restyling");
        fixture.content->clearTextSelection();
        fixture.drag();
        require(fixture.toolbarVisible() && fixture.has(Tool::Highlight),
                "reselecting the styled range restores its quick toolbar and active highlight");
        fixture.drag(1, 7, 0, false);
        if (fixture.textItem)
            fixture.textItem->ungrabMouse();
        else {
            QEvent cancel(QEvent::UngrabMouse);
            QApplication::sendEvent(fixture.content, &cancel);
        }
        require(fixture.selection().empty() && !fixture.toolbarVisible() &&
                    Access::quickTextAnnotations(fixture.window).serialize() == annotated,
                "canceling a held selection gesture does not publish quick tools or remove marks");
        fixture.content->clearTextSelection();
        require(!fixture.toolbarVisible(), "clearing the text selection hides quick tools");
        fixture.drag(1, 7, 1);
        require(!fixture.has(Tool::Highlight, false) && !fixture.has(Tool::Underline, false) &&
                    Access::quickTextAnnotations(fixture.window).serialize() == annotated,
                "selecting a new unstyled passage does not apply remembered styles");
        fixture.content->clearTextSelection();
        fixture.drag();
        require(fixture.has(Tool::Highlight),
                "reselecting annotated text recognizes its active style");
        fixture.click(Tool::Underline);
        fixture.selectAll();
        require(fixture.toolbarVisible() && fixture.has(Tool::Underline, false) &&
                    !fixture.has(Tool::Underline),
                "Select All immediately exposes partial style coverage without changing it");
        fixture.click(Tool::Underline);
        require(fixture.has(Tool::Underline),
                "clicking a partially active tool applies it throughout the selection");
        fixture.content->clearTextSelection();
        fixture.pointer(QEvent::MouseButtonDblClick, fixture.point(3), Qt::LeftButton,
                        Qt::LeftButton);
        require(!fixture.toolbarVisible() && !fixture.selection().empty() &&
                    fixture.selection().inProgress,
                "double-click selection defers quick tools until mouse release");
        fixture.pointer(QEvent::MouseButtonRelease, fixture.point(3), Qt::LeftButton, Qt::NoButton);
        require(fixture.toolbarVisible() && !fixture.selection().inProgress,
                "releasing a double-click selection shows quick tools");
        if (kind != 0) {
            QMimeData replacementMime;
            replacementMime.setText(QStringLiteral("Changed pinned text content"));
            auto replacementSnapshot = ScreenshotClipboardContentReader::snapshotMimeData(
                &replacementMime, 1.0, Qt::white);
            require(replacementSnapshot.has_value(), "snapshot replacement pin text");
            auto replacement = decodeScreenshotDropContent(std::move(*replacementSnapshot));
            require(replacement && Access::replace(fixture.window, std::move(*replacement)) &&
                        Access::quickTextAnnotations(fixture.window).empty() &&
                        Access::quickTextSnapshot(fixture.window).empty(),
                    "replacing plain or rich pin text clears the associated annotation layer");
        }
    }
}

void settingsRenderingAndPersistence() {
    SettingsGuard settings;
    Fixture fixture;
    const QImage pristine = widgetBackground(fixture.window);
    fixture.drag();
    fixture.click(Tool::Highlight);
    fixture.click(Tool::Underline);
    const auto payload = Access::quickTextAnnotations(fixture.window).serialize();
    const QImage applied = widgetBackground(fixture.window);
    require(applied != pristine, "lifecycle fixture has visible selected-text annotation ink");
    const QImage exported = exportedImage(fixture.window);
    require(settings.settings.setQuickAnnotationForSelectedText(false) &&
                fixture.toolbar() == nullptr &&
                Access::quickTextAnnotations(fixture.window).serialize() == payload &&
                widgetBackground(fixture.window) == applied,
            "disabling quick annotation hides its tools and retains visible applied styles");
    require(settings.settings.setTextSelectionOnRecognitionResults(
                QStringLiteral("only_when_displayed")) &&
                Access::quickTextAnnotations(fixture.window).serialize() == payload &&
                widgetBackground(fixture.window) == applied,
            "changing text selection policy preserves selected-text annotations");
    require(settings.settings.setTextSelectionOnRecognitionResults(QStringLiteral("always")) &&
                settings.settings.setQuickAnnotationForSelectedText(true),
            "restore canvas quick annotation availability");
    fixture.content->clearTextSelection();
    fixture.drag();
    Access::editSelectionOffscreen(fixture.window, true);
    require(Access::annotationMode(fixture.window) && fixture.toolbar() == nullptr &&
                widgetBackground(fixture.window) == pristine &&
                Access::quickTextAnnotations(fixture.window).serialize() == payload,
            "annotation mode temporarily hides text annotations without changing their model");
    require(exportedImage(fixture.window) == exported,
            "exports include text annotations while annotation mode temporarily hides them");
    Access::editSelectionOffscreen(fixture.window, false);
    require(widgetBackground(fixture.window) == applied,
            "leaving annotation mode restores selected-text annotation visibility");
    const auto record = fixture.window.persistenceSnapshot();
    require(record.textAnnotations == payload &&
                !fixture.window.imageEditingSnapshot().textAnnotations.empty(),
            "pin persistence and image-editing transfer preserve the annotation layer");
    auto restoredConfig = fixture.config;
    restoredConfig.restorePersistentState = true;
    restoredConfig.persistedTextAnnotations = record.textAnnotations;
    ScreenshotPinnedWindow restored;
    Access::hiddenSelectionOffscreen(restored, restoredConfig);
    require(Access::quickTextAnnotations(restored).serialize() == payload &&
                !Access::quickTextSnapshot(restored).empty(),
            "restoring a pin recreates text annotation state and render geometry");
    ScreenshotClipboardContent replacement;
    replacement.image = QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
    replacement.image.fill(QColor(70, 100, 120));
    require(Access::replace(fixture.window, std::move(replacement)) &&
                Access::quickTextAnnotations(fixture.window).empty() &&
                Access::quickTextSnapshot(fixture.window).empty() && !fixture.toolbarVisible(),
            "replacing pin image content clears associated text annotation state and UI");
}

void onePopoverAcrossPinsAndDisplayedTextScope() {
    SettingsGuard settings;
    Fixture first;
    first.drag();
    require(first.toolbarVisible(), "first selected pin shows quick annotation");
    Fixture second;
    second.drag();
    require(second.toolbarVisible() && first.toolbar() == nullptr,
            "a selection in another pin owns the sole quick annotation popover");
    first.window.move(first.window.pos() + QPoint(12, 0));
    require(second.toolbarVisible() && first.toolbar() == nullptr,
            "moving an older selected pin does not recreate its superseded toolbar");
    second.click(Tool::Highlight);
    const auto payload = Access::quickTextAnnotations(second.window).serialize();
    second.session->activate(ScreenshotRecognitionSessionController::Mode::Text);
    require(second.session->active() && second.toolbar() == nullptr &&
                Access::quickTextAnnotations(second.window).serialize() == payload,
            "displayed text mode suppresses quick annotation without deleting canvas marks");
    first.drag();
    require(first.toolbarVisible(), "a new selection can recreate a superseded pin's toolbar");
}

void geometryUpdatesPreserveStylesAndDraftEditsClearInk() {
    SettingsGuard settings;
    for (int kind : {0, 1, 2}) {
        Fixture fixture(kind);
        fixture.drag();
        fixture.click(Tool::Underline);
        auto* toolbar = fixture.toolbar();
        auto* button = fixture.button(Tool::Underline);
        require(button->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
                "the selected underline is active before moving its toolbar");
        const auto payload = Access::quickTextAnnotations(fixture.window).serialize();
        for (int i = 0; i < 4; ++i) {
            fixture.window.move(fixture.window.pos() + QPoint(5, 3));
            require(fixture.toolbar() == toolbar && fixture.toolbarVisible() &&
                        button->accentRole() == adqt::widgets::AdButton::AccentRole::Primary &&
                        Access::quickTextAnnotations(fixture.window).serialize() == payload,
                    "geometry-only updates retain toolbar identity and applied styles");
        }
        toolbar->updateSelectionGeometry({});
        require(!fixture.toolbarVisible(), "empty selection bounds temporarily hide the toolbar");
        fixture.window.move(fixture.window.pos() + QPoint(5, 3));
        require(fixture.toolbar() == toolbar && fixture.toolbarVisible() &&
                    button->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
                "valid selection bounds restore the toolbar with its retained styles");
        fixture.click(Tool::Underline);
        require(button->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
                "a model revision refreshes active styles after geometry-only updates");
        fixture.click(Tool::Underline);
        fixture.session->activate(ScreenshotRecognitionSessionController::Mode::Text);
        for (const auto& draft :
             {QStringLiteral("First edited draft"), QStringLiteral("Second edited draft")}) {
            require(fixture.session->editWorkflow(
                        {{QStringLiteral("action"), QStringLiteral("set_text")},
                         {QStringLiteral("text"), draft}}) &&
                        fixture.session->textDraft() == draft,
                    "successive recognition edits update the text draft");
            require(Access::quickTextAnnotations(fixture.window).empty() &&
                        Access::quickTextSnapshot(fixture.window).empty() &&
                        fixture.window.persistenceSnapshot().textAnnotations.isEmpty() &&
                        !fixture.toolbarVisible(),
                    "the first draft edit clears persisted ink and later edits keep it cleared");
        }
    }
}

void transformsAndCommittedCrop() {
    SettingsGuard settings;
    for (int kind : {0, 1, 2}) {
        Fixture fixture(kind);
        fixture.drag();
        fixture.click(Tool::Highlight);
        fixture.click(Tool::Underline);
        const auto payload = Access::quickTextAnnotations(fixture.window).serialize();
        const auto count = Access::quickTextSnapshot(fixture.window).items.size();
        fixture.content->clearTextSelection();
        for (int turn = 0; turn < 4; ++turn) {
            Access::rotateFormattedSelectionOffscreen(fixture.window);
            require(Access::quickTextAnnotations(fixture.window).serialize() == payload &&
                        Access::quickTextSnapshot(fixture.window).items.size() == count,
                    "rotation preserves logical text styles and transformed annotation geometry");
        }
        const QRectF full = Access::visibleCrop(fixture.window);
        require(
            Access::crop(fixture.window, full.adjusted(full.width() / 8, full.height() / 8,
                                                       -full.width() / 8, -full.height() / 8)) &&
                Access::quickTextAnnotations(fixture.window).serialize() == payload,
            "crop preview clips text annotations without deleting their ranges");
        Access::applyCrop(fixture.window);
        require(Access::quickTextAnnotations(fixture.window).empty() &&
                    Access::quickTextSnapshot(fixture.window).empty(),
                "committing a crop replaces source content and clears associated text marks");
    }
}

void nativeExportsPreserveTextAnnotations() {
    SettingsGuard settings;
    for (const int kind : {0, 2}) {
        for (const int variant : {0, 1, 2}) {
            Fixture fixture(kind, kind == 2 ? 2.0 : 1.0);
            fixture.drag();
            fixture.click(Tool::Highlight);
            ScreenshotTextAnnotationStyle style;
            style.color = QColor(255, 0, 0, 128);
            fixture.toolbar()->styleEdited(Tool::Highlight, style,
                                           ScreenshotTextAnnotationStylePropertyColor);
            fixture.click(Tool::Underline);
            fixture.toolbar()->styleEdited(Tool::Underline, style,
                                           ScreenshotTextAnnotationStylePropertyColor);
            fixture.content->clearTextSelection();
            if (variant == 1) {
                Access::rotateFormattedSelectionOffscreen(fixture.window);
                Access::setFractionalScale(fixture.window, 125);
            } else if (variant == 2) {
                const auto full = Access::visibleCrop(fixture.window);
                require(Access::crop(fixture.window,
                                     full.adjusted(full.width() / 8, full.height() / 8,
                                                   -full.width() / 8, -full.height() / 8)),
                        "crop the annotated native-export viewport");
            }
            const int opacity = variant == 1 ? 60 : 100;
            Access::setGeneralOpacity(fixture.window, opacity);
            fixture.session->activate(ScreenshotRecognitionSessionController::Mode::Text);
            QApplication::processEvents();
            auto* canvas = fixture.window.findChild<SnowCanvasWidget*>();
            auto* view = fixture.content->findChild<QGraphicsView*>(
                kind == 0 ? QStringLiteral("snowShotOcrTextLayer")
                          : QStringLiteral("screenshotClipboardText"));
            require(view && view->isVisible() && canvas,
                    "native annotation export uses the visible canvas text page");
            const auto source = fixture.window.imageEditingSnapshot();
            const auto marks = Access::quickTextSnapshot(fixture.window);
            require(!source.image.isNull() && !marks.empty(),
                    "native export retains the pristine image and separate text marks");
            for (const int filter : {0, 1, 2}) {
                if (kind != 0 && filter != 0)
                    continue;
                QImage filtered;
                QRectF filteredRect;
                if (filter != 0) {
                    filteredRect = source.canvasRect;
                    if (filter == 2)
                        filteredRect.setLeft(filteredRect.center().x());
                    filtered = QImage(filteredRect.size().toSize(), source.image.format());
                    filtered.fill(QColor(200, 220, 240));
                    Access::renderer(fixture.window).setOcrFilteredImage(filtered, filteredRect);
                } else {
                    Access::renderer(fixture.window).clearOcrFilteredImage();
                }
                // Build the reference from pristine pixels, then add one vector pass in the
                // visible viewport's coordinates. Apply opacity only to the final composition.
                QImage expected = fixture.content->printViewportSnapshot(
                    source.image, source.canvasRect, filtered, filteredRect);
                QWidget* viewport = view->viewport();
                const QPoint offset = viewport->mapFromGlobal(canvas->mapToGlobal(QPoint()));
                const auto mapping =
                    canvas->canvasToViewTransform() *
                    QTransform::fromTranslate(offset.x(), offset.y()) *
                    QTransform::fromScale(qreal(expected.width()) / viewport->width(),
                                          qreal(expected.height()) / viewport->height());
                {
                    QPainter painter(&expected);
                    paintScreenshotTextAnnotations(painter, marks, mapping);
                }
                expected = ScreenshotResultCompositor::compose(expected, {}, 1.0, opacity / 100.0);

                // A completed Copy Viewport result must not be reused as the recognition
                // backdrop, or semitransparent marks will be painted twice.
                auto cached = Access::printArtifact(fixture.window);
                QImage complete;
                require(cached && cached->requestImage(
                                      &fixture.window,
                                      [&](auto result) { complete = std::move(result.image); }),
                        "warm the annotated viewport cache before native export");
                translation_tests::waitUntil([&] { return !complete.isNull(); },
                                             "complete the annotated viewport cache entry");
                QImage printed, shared;
                ScreenshotPrintService printer([&](QWidget*, QImage image, auto completion) {
                    printed = std::move(image);
                    completion({ScreenshotPrintService::Status::Cancelled, {}});
                });
                Access::print(fixture.window, &printer);
                translation_tests::waitUntil([&] { return !Access::printPending(fixture.window); },
                                             "finish annotated recognition printing");
                require(!printed.isNull() &&
                            printed == ScreenshotPrintService::opaqueImage(expected),
                        "Print preserves each text mark once after OCR filtering, transforms, crop "
                        "and opacity");
                ScreenshotAirDropService sharing([&](QWidget*, QString path, auto completion) {
                    shared = QImage(path);
                    completion({ScreenshotAirDropService::Status::Shared, {}});
                });
                Access::airDrop(fixture.window, &sharing);
                translation_tests::waitUntil(
                    [&] { return !Access::airDropPending(fixture.window); },
                    "finish annotated recognition sharing");
                require(
                    !shared.isNull() && shared.convertToFormat(QImage::Format_ARGB32) ==
                                            expected.convertToFormat(QImage::Format_ARGB32),
                    "AirDrop preserves text marks and final alpha in the same viewport as Print");
            }
        }
    }
}

void croppedMasksSurviveImageEditingTransfer() {
    using namespace snow_shot::presentation;
    SettingsGuard settings;
    const auto preparation = captureScreenshotPinnedTextPreparation();
    for (const int kind : {0, 2}) {
        Fixture fixture(kind, 1.0, true);
        fixture.drag();
        fixture.click(Tool::Highlight);
        fixture.click(Tool::Underline);
        fixture.content->clearTextSelection();
        const auto full = Access::visibleCrop(fixture.window);
        require(Access::crop(fixture.window, full.adjusted(0, 0, -20, -10)),
                "crop the masked text annotation transfer fixture");
        const auto expected = exportedImage(fixture.window);
        require(!expected.isNull() && expected.pixelColor(10, 10).alpha() == 0,
                "the cropped pin export retains its excluded left half");
        const auto record = fixture.window.persistenceSnapshot();
        for (const bool open : {true, false}) {
            const auto source = open ? fixture.window.imageEditingSnapshot()
                                     : readImageEditingPinned(record, preparation);
            ImageEditingWindow editor;
            bool loaded = false;
            QObject::connect(&editor, &ImageEditingWindow::loaded, &editor, [&](bool success) {
                require(success, "cropped masked text annotations import successfully");
                loaded = true;
            });
            editor.present(source);
            translation_tests::waitUntil([&] { return loaded; },
                                         "complete cropped masked text annotation import");
            require(
                editor.document()->image().convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
                    expected.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                "open and stored cropped pins preserve text marks inside their mask exactly once");
            require(source.textAnnotations.empty() && !source.resultStyle &&
                        !source.samplingSources.isEmpty() &&
                        source.samplingSources.front().image ==
                            fixture.config.imageSource.materializedImage,
                    "masked imports consume baked text marks and retain pristine sampling donors");
            editor.shutdown();
        }
        require(fixture.window.persistenceSnapshot().textAnnotations == record.textAnnotations,
                "importing a cropped mask leaves the original pin's text annotations unchanged");
    }
}

void sourceFingerprintsAreDeferredUntilAnnotation() {
    SettingsGuard guard;
    for (int kind : {0, 1, 2}) {
        require(guard.settings.setQuickAnnotationForSelectedText(false),
                "disable quick annotation before binding an unannotated source");
        Fixture fixture(kind);
        require(Access::quickTextAnnotations(fixture.window).fingerprint().isEmpty(),
                "binding an unannotated OCR or formatted pin does not hash its source");
        fixture.drag();
        require(!fixture.toolbarVisible() &&
                    Access::quickTextAnnotations(fixture.window).fingerprint().isEmpty(),
                "ordinary selection with quick annotation disabled does not hash the source");
        require(guard.settings.setQuickAnnotationForSelectedText(true),
                "enable quick annotation for the existing selection");
        require(fixture.toolbarVisible() &&
                    Access::quickTextAnnotations(fixture.window).fingerprint().isEmpty(),
                "opening the toolbar still does not serialize source content");
        fixture.click(Tool::Underline);
        const auto fingerprint = Access::quickTextAnnotations(fixture.window).fingerprint();
        require(fixture.has(Tool::Underline) && !fingerprint.isEmpty(),
                "the first annotation binds a persistent source fingerprint");
        fixture.click(Tool::Underline);
        fixture.click(Tool::Underline);
        require(Access::quickTextAnnotations(fixture.window).fingerprint() == fingerprint &&
                    !fixture.window.persistenceSnapshot().textAnnotations.isEmpty(),
                "removing and reapplying ink preserves its valid source binding and persistence");
    }
}

void run() {
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load deterministic pinned selection font");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    onDemandToolbarLifetime();
    immediateSelectionAndStyleRules();
    settingsRenderingAndPersistence();
    onePopoverAcrossPinsAndDisplayedTextScope();
    geometryUpdatesPreserveStylesAndDraftEditsClearInk();
    transformsAndCommittedCrop();
    nativeExportsPreserveTextAnnotations();
    croppedMasksSurviveImageEditingTransfer();
    sourceFingerprintsAreDeferredUntilAnnotation();
}
} // namespace pinned_quick_text_annotation_tests

#endif
