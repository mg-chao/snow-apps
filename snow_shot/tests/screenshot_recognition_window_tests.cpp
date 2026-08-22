#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshottableeditor.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "theme/theme_manager.h"
#include "widgets/input_text_edit.h"
#include "widgets/scroll_area.h"
#include "widgets/spin.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFrame>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QGraphicsItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPainter>
#include <QScrollBar>
#include <QScreen>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QUrl>
#include <QWindow>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void clickCell(ScreenshotTableEditor& editor, int row, int column) {
    const QModelIndex index = editor.model()->index(row, column);
    const QPoint localPosition = editor.visualRect(index).center();
    const QPoint globalPosition = editor.viewport()->mapToGlobal(localPosition);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(localPosition), QPointF(globalPosition),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(localPosition), QPointF(globalPosition),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(editor.viewport(), &release);
    QApplication::processEvents();
}

void processEditorClose() {
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

QImage renderTableEditor(ScreenshotTableEditor& editor, qreal devicePixelRatio) {
    const QSize pixelSize(qCeil(editor.width() * devicePixelRatio),
                          qCeil(editor.height() * devicePixelRatio));
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    editor.render(&painter);
    return image;
}

double averageLightness(const QImage& image) {
    if (image.isNull()) {
        return 0.0;
    }
    quint64 total = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            total += static_cast<quint64>(qGray(image.pixel(x, y)));
        }
    }
    return static_cast<double>(total) / static_cast<double>(image.width() * image.height());
}

void embeddedRecognitionWindowPreservesParentSurfaceWithVisibleTextLayer() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QColor background(24, 72, 120, 255);
    QWidget host;
    host.resize(160, 90);
    QPalette palette = host.palette();
    palette.setColor(QPalette::Window, background);
    host.setPalette(palette);
    host.setAutoFillBackground(true);
    host.show();

    ScreenshotRecognitionWindow recognition(
        ScreenshotRecognitionWindowActions{}, &host,
        ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(recognition.present(ScreenshotRecognitionWindow::Config{
                screen,
                &host,
                host.rect(),
                QRectF(QPointF(), QSizeF(host.size())),
                ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
            }),
            "an embedded recognition window should accept its parent geometry");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = host.rect();
    presentation->filledImage = QImage(host.size(), QImage::Format_RGBA8888);
    presentation->filledImage.fill(QColor(220, 240, 255, 80));
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Embedded OCR");
    line.foreground = Qt::black;
    line.quad = QPolygonF({QPointF(45.0, 35.0), QPointF(115.0, 35.0),
                           QPointF(115.0, 55.0), QPointF(45.0, 55.0)});
    presentation->lines.push_back(line);
    presentation->prepareForRendering();
    recognition.setOcrPresentation(std::move(presentation));
    QApplication::processEvents();
    auto* textLayer = recognition.findChild<QGraphicsView*>(
        QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && textLayer->isVisible(),
            "the embedded recognition text layer should participate in composition");

    QImage rendered(host.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    host.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();

    require(rendered.pixelColor(QPoint(10, 10)) == background,
            "an embedded recognition surface must not replace its parent's painted pixels");
}

void recognitionWindowCanExtendBeyondItsDpiScreen() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();

    const QRect screenGeometry = screen->geometry();
    const QRect crossScreenSelection(screenGeometry.right() - 100, screenGeometry.top() + 40,
                                     240, 120);
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                crossScreenSelection,
                QRectF(0.0, 0.0, 240.0, 120.0),
            }),
            "a recognition window should accept a selection spanning screen boundaries");
    QApplication::processEvents();
    require(window.geometry() == crossScreenSelection && window.parentWidget() == nullptr,
            "cross-screen recognition geometry must not be clipped by an overlay parent");
    require(window.windowHandle() != nullptr && window.windowHandle()->screen() == screen,
            "a cross-screen recognition window should retain the selection screen's DPI");
    window.hide();
}

#if defined(Q_OS_WIN) || defined(_WIN32)
QRect nativeClientGeometry(const QWidget& widget) {
    const DPI_AWARENESS_CONTEXT previousContext =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND hwnd = reinterpret_cast<HWND>(widget.winId());
    RECT clientRect{};
    POINT topLeft{};
    const bool queried =
        GetClientRect(hwnd, &clientRect) != FALSE && ClientToScreen(hwnd, &topLeft) != FALSE;
    if (previousContext != nullptr) {
        static_cast<void>(SetThreadDpiAwarenessContext(previousContext));
    }
    if (!queried) {
        return {};
    }
    return QRect(topLeft.x, topLeft.y, clientRect.right - clientRect.left,
                 clientRect.bottom - clientRect.top);
}

HWND windowAtPhysicalPoint(const POINT& point) {
    const DPI_AWARENESS_CONTEXT previousContext =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND window = WindowFromPoint(point);
    if (previousContext != nullptr) {
        static_cast<void>(SetThreadDpiAwarenessContext(previousContext));
    }
    return window;
}

#endif

void recognitionWindowUsesOrdinaryQtWindowBehavior() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect nativeSelection(physicalScreen.topLeft() + QPoint(31, 29), QSize(420, 240));
    const QRect logicalSelection =
        ScreenshotGeometryMapper::logicalRectForPhysicalRect(nativeSelection, screen);
    const QRectF canvasSelection(QPointF(0.0, 0.0), QSizeF(nativeSelection.size()));
    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();
    overlayHost.raise();
    int recognitionCancelCalls = 0;
    int textEditedCalls = 0;
    ScreenshotRecognitionWindowActions actions;
    actions.handleCancel = [&recognitionCancelCalls]() { ++recognitionCancelCalls; };
    actions.handleTextEdited = [&textEditedCalls](const QString&) { ++textEditedCalls; };
    int tableStateChanges = 0;
    ScreenshotTableCommandState latestTableState;
    actions.handleTableCommandStateChanged =
        [&tableStateChanges, &latestTableState](const ScreenshotTableCommandState& state) {
            ++tableStateChanges;
            latestTableState = state;
        };
    int rejectedOperations = 0;
    actions.handleTableOperationRejected = [&rejectedOperations](const QString&) {
        ++rejectedOperations;
    };
    ScreenshotRecognitionWindow window(std::move(actions));
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                logicalSelection,
                canvasSelection,
            }),
            "recognition window should present a valid selection");
    QApplication::processEvents();

    require(window.isWindow() && window.parentWidget() == nullptr,
            "the recognition host should be an ordinary top-level Qt window");
    require(window.windowHandle() != nullptr &&
                window.windowHandle()->transientParent() == overlayHost.windowHandle(),
            "the screenshot overlay should own recognition stacking through Qt");
    require(window.geometry() == logicalSelection,
            "the recognition window should preserve the mapped selection geometry");
    require(window.windowHandle() != nullptr && window.windowHandle()->screen() == screen,
            "the recognition window DPI should come from the configured selection screen");
#if defined(Q_OS_WIN) || defined(_WIN32)
    const QRect actualClientGeometry = nativeClientGeometry(window);
    require(actualClientGeometry.isValid() && !actualClientGeometry.isEmpty(),
            "the recognition window should expose a valid native client surface");
#endif
    require(window.findChild<SnowCanvasWidget*>() == nullptr,
            "the recognition window should not create a screenshot canvas");
    require(window.testAttribute(Qt::WA_TranslucentBackground) && !window.autoFillBackground(),
            "the recognition window should have a visually transparent, input-bearing surface");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = canvasSelection.toAlignedRect();
    presentation->filledImage =
        QImage(presentation->selection.size(), QImage::Format_RGBA8888);
    presentation->filledImage.fill(Qt::white);
    ScreenshotOcrLine recognizedLine;
    recognizedLine.text = QStringLiteral("Selectable text");
    recognizedLine.foreground = Qt::black;
    const QPointF selectionCenter = canvasSelection.center();
    recognizedLine.quad = QPolygonF({
        selectionCenter + QPointF(-90.0, -18.0),
        selectionCenter + QPointF(90.0, -18.0),
        selectionCenter + QPointF(90.0, 18.0),
        selectionCenter + QPointF(-90.0, 18.0),
    });
    presentation->lines.push_back(recognizedLine);
    presentation->prepareForRendering();
    window.setOcrPresentation(presentation);
    QApplication::processEvents();
    auto* textLayer = window.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && textLayer->isVisible() && textLayer->scene() != nullptr &&
                textLayer->scene()->items().size() == 1,
            "selectable OCR text should be hosted directly by the transparent window");
    require(textLayer->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                textLayer->viewport()->testAttribute(Qt::WA_TransparentForMouseEvents),
            "the OCR rendering child should leave all pointer input to its recognition window");

    const auto localForCanvas = [&window, &canvasSelection](const QPointF& canvasPosition) {
        return QPointF((canvasPosition.x() - canvasSelection.left()) * window.width() /
                           canvasSelection.width(),
                       (canvasPosition.y() - canvasSelection.top()) * window.height() /
                           canvasSelection.height());
    };
    const QPointF localStart = localForCanvas(selectionCenter + QPointF(-70.0, 0.0));
    const QPointF localEnd = localForCanvas(selectionCenter + QPointF(70.0, 0.0));
    QMouseEvent recognitionPress(QEvent::MouseButtonPress, localStart,
                                 window.mapToGlobal(localStart.toPoint()), Qt::LeftButton,
                                 Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &recognitionPress);
    require(presentation->textSelectionActive(),
            "the recognition window should begin OCR selection without controller forwarding");
    QMouseEvent recognitionMove(QEvent::MouseMove, localEnd,
                                window.mapToGlobal(localEnd.toPoint()), Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &recognitionMove);
    QMouseEvent recognitionRelease(QEvent::MouseButtonRelease, localEnd,
                                   window.mapToGlobal(localEnd.toPoint()), Qt::LeftButton,
                                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &recognitionRelease);
    require(!presentation->textSelectionActive() && !presentation->selectedText().isEmpty(),
            "the recognition window should update and finish OCR selection locally");

    QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(&window, &selectAll);
    require(presentation->selectedText() == recognizedLine.text,
            "Select All should be handled locally by the recognition window");

    const QPointF blankCanvasPosition = canvasSelection.topLeft() + QPointF(12.0, 12.0);
    const QPointF blankLocalPosition = localForCanvas(blankCanvasPosition);
#if defined(Q_OS_WIN) || defined(_WIN32)
    window.repaint();
    QApplication::processEvents();
    const QRect actualNative = nativeClientGeometry(window);
    const POINT blankNativePosition{
        actualNative.left() + qRound(blankLocalPosition.x() * actualNative.width() / window.width()),
        actualNative.top() + qRound(blankLocalPosition.y() * actualNative.height() / window.height()),
    };
    const HWND blankOwner = windowAtPhysicalPoint(blankNativePosition);
    const HWND recognitionHwnd = reinterpret_cast<HWND>(window.winId());
    require(blankOwner == recognitionHwnd,
            "transparent blank pixels should remain part of the recognition input surface");
#endif
    QMouseEvent blankPress(QEvent::MouseButtonPress, blankLocalPosition,
                           window.mapToGlobal(blankLocalPosition.toPoint()), Qt::LeftButton,
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &blankPress);
    QMouseEvent blankRelease(QEvent::MouseButtonRelease, blankLocalPosition,
                             window.mapToGlobal(blankLocalPosition.toPoint()), Qt::LeftButton,
                             Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &blankRelease);
    require(!presentation->hasTextSelection(),
            "clicking a blank OCR area should clear the existing text selection");

    // Selection is custom presentation state, so losing focus must explicitly clear it.
    QKeyEvent selectAllAgain(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(&window, &selectAllAgain);
    require(presentation->hasTextSelection(),
            "Select All should establish a selection before the focus-loss check");
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QApplication::sendEvent(&window, &focusOut);
    require(!presentation->hasTextSelection(),
            "losing recognition-window focus should clear the existing text selection");

    QTextDocument editableDocument;
    QFont oversizedFont = editableDocument.defaultFont();
    oversizedFont.setPixelSize(32);
    editableDocument.setDefaultFont(oversizedFont);
    editableDocument.setPlainText(QStringLiteral("Editable recognized text"));
    window.showTextEditor(&editableDocument);
    QApplication::processEvents();
    auto* textEditor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    auto* textEditorContainer =
        window.findChild<QWidget*>(QStringLiteral("screenshotOcrEditorContainer"));
    auto* adTextEditor = qobject_cast<adqt::widgets::AdTextEdit*>(textEditor);
    require(textEditorContainer != nullptr && textEditor != nullptr &&
                textEditor->parentWidget() == textEditorContainer &&
                textEditorContainer->palette().color(QPalette::Window) ==
                    adqt::theme::ThemeManager::instance()
                        .resolveTheme(textEditorContainer)
                        .colorBgContainer &&
                adTextEditor != nullptr && textEditor->frameStyle() == QFrame::NoFrame &&
                adTextEditor->variant() == adqt::widgets::AdTextEdit::Variant::Borderless,
            "the OCR text editor should use a themed borderless container");
    auto* overlayScrollBar = textEditor->findChild<QScrollBar*>(
        QStringLiteral("adtextarea-overlay-vbar"));
    require(overlayScrollBar != nullptr && textEditor->verticalScrollBar()->isHidden(),
            "the OCR text editor should use an overlay scrollbar without reserving layout width");
    const QRect viewportGeometry = textEditor->viewport()->geometry();
    const int viewportLeftInset = viewportGeometry.left();
    const int viewportRightInset = textEditor->width() - viewportGeometry.right() - 1;
    require(viewportLeftInset > 0 && viewportRightInset > 0 &&
                viewportLeftInset == viewportRightInset &&
                editableDocument.documentMargin() == 0.0,
            "the OCR text editor should use symmetric viewport content margins");
    const int viewportWidthBeforeOverflow = textEditor->viewport()->width();
    editableDocument.setPlainText(QString(5000, QLatin1Char('x')));
    QApplication::processEvents();
    require(overlayScrollBar->isVisible() &&
                textEditor->viewport()->width() == viewportWidthBeforeOverflow,
            "the OCR overlay scrollbar should not change the text viewport width");
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(textEditor);
    const int themeFontSize = qRound(theme.fontSize);
    require(textEditor->font().pixelSize() == themeFontSize &&
                editableDocument.defaultFont().pixelSize() == themeFontSize,
            "the OCR text editor should use the theme standard font size");

    window.showTextEditor(&editableDocument, true, true);
    QApplication::processEvents();
    auto* translationSpin = window.findChild<adqt::widgets::AdSpin*>(
        QStringLiteral("screenshotOcrTranslationSpin"));
    require(textEditor->isReadOnly() && translationSpin != nullptr &&
                translationSpin->isVisible() && translationSpin->spinning(),
            "streaming translation should make the editor read-only and show its spinner");
    window.setTextEditorStreaming(false);
    QApplication::processEvents();
    require(!textEditor->isReadOnly() && !translationSpin->isVisible() &&
                !translationSpin->spinning(),
            "translation completion should restore editing and hide the spinner");

    textEditedCalls = 0;
    textEditedCalls = 0;
    window.hideTextEditor();
    require(textEditedCalls == 0,
            "hiding the OCR text editor should not emit a spurious empty text edit");

    auto session = std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("A\tB\nC\tD")));
    window.setTableSession(session);
    QApplication::processEvents();
    auto* editor =
        window.findChild<ScreenshotTableEditor*>(QStringLiteral("snowShotRecognizedTable"));
    require(editor != nullptr && editor->geometry() == window.rect(),
            "the table editor should fill the recognition window");
    const auto tableTheme = adqt::theme::ThemeManager::instance().resolveTheme(editor);
    const int tablePadding = std::max(0, qRound(tableTheme.sizeSM));
    require(editor->viewport()->geometry().topLeft() == QPoint(tablePadding, tablePadding),
            "the table viewport should keep theme-driven outer padding");
    auto* horizontalScrollBar =
        qobject_cast<adqt::widgets::AdScrollBar*>(editor->horizontalScrollBar());
    auto* verticalScrollBar =
        qobject_cast<adqt::widgets::AdScrollBar*>(editor->verticalScrollBar());
    require(horizontalScrollBar != nullptr && verticalScrollBar != nullptr,
            "the table should use default ant_design_qt scrollbars on both axes");
    require(textLayer->isHidden(), "table mode should hide selectable OCR text");
    require(editor->rowSpan(0, 0) == 1 && editor->columnSpan(0, 0) == 1,
            "unmerged recognition cells should use normal table geometry");

    const QSize viewportBeforeEdit = editor->viewport()->size();
    const int firstColumnWidthBeforeEdit = editor->columnWidth(0);
    const int secondColumnWidthBeforeEdit = editor->columnWidth(1);
    const int firstRowHeightBeforeEdit = editor->rowHeight(0);

    clickCell(*editor, 0, 0);
    auto* cellEditor =
        editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr && editor->isEditingCell(),
            "one click should open an inline editor directly over the selected cell");
    auto* cellScrollBar =
        qobject_cast<adqt::widgets::AdScrollBar*>(cellEditor->verticalScrollBar());
    require(cellScrollBar != nullptr &&
                cellScrollBar->scrollBarThickness() == verticalScrollBar->scrollBarThickness(),
            "inline table textareas should use the default ant_design_qt scrollbar");
    require(editor->viewport()->size() == viewportBeforeEdit &&
                editor->columnWidth(0) == firstColumnWidthBeforeEdit &&
                editor->columnWidth(1) == secondColumnWidthBeforeEdit &&
                editor->rowHeight(0) == firstRowHeightBeforeEdit,
            "opening an inline editor should preserve table and viewport geometry");
    require(cellEditor->document()->documentMargin() == 0.0,
            "inline table editors should remove the document's implicit text margin");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &escape);
    processEditorClose();
    require(!editor->isEditingCell() && recognitionCancelCalls == 0,
            "Escape should cancel an active cell edit before reaching screenshot cancellation");

    clickCell(*editor, 0, 0);
    cellEditor = editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline editor should reopen after cancel");
    cellEditor->setPlainText(QString(240, QLatin1Char('L')));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &enter);
    processEditorClose();
    require(session->document.cellText(0, 0) == QString(240, QLatin1Char('L')) &&
                editor->currentIndex().row() == 1 && editor->currentIndex().column() == 0,
            "Enter should commit the edit and continue vertically in the same column");
    require(editor->viewport()->size() == viewportBeforeEdit &&
                editor->columnWidth(0) == firstColumnWidthBeforeEdit &&
                editor->columnWidth(1) == secondColumnWidthBeforeEdit &&
                editor->rowHeight(0) == firstRowHeightBeforeEdit,
            "committing a cell edit should not resize the table around the next editor");
    require(editor->cancelActiveEdit(), "the continued inline edit should be cancellable");
    processEditorClose();

    window.resetTable();
    clickCell(*editor, 0, 0);
    cellEditor = editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline editor should open for horizontal navigation");
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &tab);
    processEditorClose();
    require(editor->currentIndex().row() == 0 && editor->currentIndex().column() == 1 &&
                editor->isEditingCell(),
            "Tab should commit and continue horizontally in the next column");
    require(editor->cancelActiveEdit(), "the horizontally continued edit should be cancellable");
    processEditorClose();

    clickCell(*editor, 0, 0);
    cellEditor = editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline editor should open before toolbar-style copying");
    cellEditor->setPlainText(QStringLiteral("Edited before copy"));
    window.commitActiveTableEdit();
    require(session->document.cellText(0, 0) == QStringLiteral("Edited before copy"),
            "copy preparation should synchronously commit the active cell editor");
    require(!editor->isEditingCell(),
            "copy preparation should synchronously close the active cell editor");
    processEditorClose();
    window.resetTable();

    editor->selectAll();
    QApplication::processEvents();
    require(window.tableCommandState().canMerge,
            "a rectangular multi-cell selection should enable Merge");
    window.mergeTableSelection();
    require(session->document.anchorCellAt(0, 0)->rowSpan == 2 &&
                session->document.anchorCellAt(0, 0)->columnSpan == 2 &&
                editor->rowSpan(0, 0) == 2 && editor->columnSpan(0, 0) == 2,
            "Merge should update both document spans and visible table geometry");
    require(window.tableCommandState().canSplit && window.tableCommandState().canUndo,
            "a merged table should enable Split and Undo");

    auto& themeManager = adqt::theme::ThemeManager::instance();
    const adqt::theme::AdTheme originalTheme = themeManager.theme();
    themeManager.setPreset(adqt::theme::ThemeScheme::Light, adqt::theme::ThemeDensity::Compact);
    QApplication::processEvents();
    const QImage light1x = renderTableEditor(*editor, 1.0);
    const QImage light2x = renderTableEditor(*editor, 2.0);
    themeManager.setPreset(adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeDensity::Compact);
    QApplication::processEvents();
    const QImage dark1x = renderTableEditor(*editor, 1.0);
    const QImage dark2x = renderTableEditor(*editor, 2.0);
    themeManager.setTheme(originalTheme);
    QApplication::processEvents();
    require(!light1x.isNull() && !dark1x.isNull() &&
                averageLightness(light1x) > averageLightness(dark1x) + 40.0,
            "light and dark theme tokens should produce distinct nonblank table surfaces");
    require(light2x.size() == light1x.size() * 2 && dark2x.size() == dark1x.size() * 2,
            "table rendering should preserve logical geometry at high device pixel ratios");
    const QString snapshotDirectory = qEnvironmentVariable("SNOW_SHOT_TABLE_SNAPSHOT_DIR");
    if (!snapshotDirectory.isEmpty() && QDir().mkpath(snapshotDirectory)) {
        light1x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-light-1x.png")));
        light2x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-light-2x.png")));
        dark1x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-dark-1x.png")));
        dark2x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-dark-2x.png")));
    }

    window.undoTableEdit();
    require(session->document == session->baseline && window.tableCommandState().canRedo,
            "Undo should restore the recognized baseline and enable Redo");
    window.redoTableEdit();
    require(session->document.anchorCellAt(0, 0)->rowSpan == 2,
            "Redo should restore the merged table");
    window.splitTableSelection();
    require(session->document.anchorCellAt(0, 0)->rowSpan == 1 &&
                session->document.cellText(0, 0) == QStringLiteral("A\nB\nC\nD") &&
                session->document.cellText(1, 1).isEmpty(),
            "Split should retain merged text at top-left and blank uncovered cells");
    require(window.tableCommandState().canReset,
            "editing recognized structure should enable Reset");
    window.resetTable();
    require(session->document == session->baseline && !window.tableCommandState().canReset,
            "Reset should restore recognized values and spans without changing dimensions");

    editor->selectAll();
    editor->copySelection();
    require(QApplication::clipboard()->text() == QStringLiteral("A\tB\nC\tD"),
            "in-table Copy should export the selected range as TSV");
    editor->setCurrentIndex(editor->model()->index(1, 1));
    editor->pasteSelection();
    require(rejectedOperations == 1 && session->document == session->baseline,
            "Paste should reject data that does not fit fixed recognized dimensions");
    editor->selectAll();
    editor->clearSelectionContents();
    editor->setCurrentIndex(editor->model()->index(0, 0));
    editor->pasteSelection();
    require(session->document.rowCount() == 2 && session->document.columnCount() == 2 &&
                session->document.cellText(1, 1) == QStringLiteral("D"),
            "a fitting paste should replace cells without inserting rows or columns");
    window.undoTableEdit();
    require(session->document.toPlainText() == QStringLiteral("\t\n\t"),
            "a pasted range should be reversible as one undo command");
    window.redoTableEdit();
    require(session->document == session->baseline,
            "Redo should restore the pasted range without changing dimensions");
    window.undoTableEdit();
    window.undoTableEdit();
    require(session->document == session->baseline,
            "undoing Paste and Clear should return to the recognized baseline");

    window.clearTableSession();
    window.setTableSession(session);
    QApplication::processEvents();
    editor = window.findChild<ScreenshotTableEditor*>(QStringLiteral("snowShotRecognizedTable"));
    require(editor != nullptr && editor->commandState().canRedo &&
                editor->selectedRange().isValid(),
            "table re-entry should restore the session selection and undo history");
    require(tableStateChanges > 0 && latestTableState.hasSelection,
            "table command availability should be published to the toolbar controller");

    window.hide();
    QApplication::processEvents();
}

QGraphicsTextItem* formattedTextItem(QGraphicsView* layer) {
    if (layer == nullptr || layer->scene() == nullptr) {
        return nullptr;
    }
    for (QGraphicsItem* item : layer->scene()->items()) {
        auto* textItem = dynamic_cast<QGraphicsTextItem*>(item);
        if (textItem != nullptr &&
            textItem->objectName() == QStringLiteral("screenshotClipboardTextItem")) {
            return textItem;
        }
    }
    return nullptr;
}

QRect nonWhitePixelBounds(const QImage& image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() < 250 || color.green() < 250 || color.blue() < 250) {
                bounds = bounds.isNull() ? QRect(x, y, 1, 1) : bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

void shortRecognitionWindowPreservesExactSelectionGeometryAcrossModes() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    constexpr int selectionWidth = 260;
    constexpr int selectionHeight = 24;
    const QRect selectionGeometry(screen->geometry().topLeft() + QPoint(40, 40),
                                  QSize(selectionWidth, selectionHeight));
    const QRectF canvasSelection(QPointF(), QSizeF(selectionWidth, selectionHeight));

    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();

    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                selectionGeometry,
                canvasSelection,
            }),
            "a short recognition selection should be presentable");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = canvasSelection.toAlignedRect();
    presentation->filledImage =
        QImage(presentation->selection.size(), QImage::Format_RGBA8888);
    presentation->filledImage.fill(Qt::white);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Short OCR");
    line.foreground = Qt::black;
    line.quad = QPolygonF({
        QPointF(70.0, 4.0),
        QPointF(190.0, 4.0),
        QPointF(190.0, 20.0),
        QPointF(70.0, 20.0),
    });
    presentation->lines.push_back(line);
    presentation->prepareForRendering();
    window.setOcrPresentation(presentation);
    QApplication::processEvents();

    auto* textLayer =
        window.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(window.geometry() == selectionGeometry && textLayer != nullptr &&
                textLayer->geometry() == window.rect(),
            "OCR display mode must not enlarge a short screenshot selection");
    const QList<QGraphicsItem*> textItems = textLayer->scene()->items();
    require(textItems.size() == 1 &&
                std::abs(textItems.constFirst()->sceneBoundingRect().center().y() -
                         selectionHeight / 2.0) < 0.5,
            "OCR text must retain its canvas vertical position in a short selection");

    QTextDocument document;
    document.setPlainText(QStringLiteral("Editable short OCR text"));
    window.showTextEditor(&document);
    QApplication::processEvents();

    auto* editorContainer =
        window.findChild<QWidget*>(QStringLiteral("screenshotOcrEditorContainer"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(window.geometry() == selectionGeometry && editorContainer != nullptr &&
                editorContainer->geometry() == window.rect() && editor != nullptr &&
                editor->geometry() == editorContainer->rect(),
            "edit mode must remain exactly within a short screenshot selection");
    window.hideTextEditor();
}

void formattedClipboardTextUsesASelectableQtDocument() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QWidget host;
    host.resize(360, 160);
    host.show();
    const QSizeF canvasSize(host.width() * 2.0, host.height() * 2.0);
    ScreenshotRecognitionWindow window(
        ScreenshotRecognitionWindowActions{}, &host,
        ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &host,
                host.rect(),
                QRectF(QPointF(), canvasSize),
                ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
                2.0,
            }),
            "the formatted clipboard recognition surface should present");

    auto document = std::make_shared<QTextDocument>();
    document->setDocumentMargin(0.0);
    document->setHtml(QStringLiteral(
        "<p style=\"font-size: 40px; margin: 0\"><b>Formatted</b> clipboard text</p>"));
    document->setTextWidth(host.width());
    QImage canonicalImage(canvasSize.toSize(), QImage::Format_ARGB32_Premultiplied);
    canonicalImage.setDevicePixelRatio(2.0);
    canonicalImage.fill(Qt::white);
    QPainter canonicalPainter(&canonicalImage);
    canonicalPainter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                                    QPainter::SmoothPixmapTransform);
    document->drawContents(&canonicalPainter);
    canonicalPainter.end();
    window.showFormattedText(document);
    QApplication::processEvents();

    auto* textLayer = window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    auto* textItem = formattedTextItem(textLayer);
    const Qt::TextInteractionFlags selectionFlags =
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard;
    require(textLayer != nullptr && textLayer->isVisible() && textLayer->isInteractive() &&
                textItem != nullptr && textItem->document() == document.get() &&
                (textItem->textInteractionFlags() & selectionFlags) == selectionFlags &&
                !textItem->openExternalLinks(),
            "formatted clipboard text should retain its Qt document and allow direct selection");
    require(textItem->toPlainText().contains(QStringLiteral("Formatted clipboard text")),
            "the selectable clipboard surface should preserve document text");
    require(std::abs(textLayer->transform().m11() - 0.5) < 0.001 &&
                std::abs(textLayer->transform().m22() - 0.5) < 0.001 &&
                qFuzzyCompare(textItem->scale(), 2.0),
            "formatted clipboard text should map canonical image pixels through the canvas "
            "transform using the owning display DPR");

    QPoint requestedContextMenuPosition;
    int contextMenuRequestCount = 0;
    QObject::connect(&window, &ScreenshotRecognitionWindow::embeddedContextMenuRequested,
                     &window, [&](const QPoint& globalPosition) {
                         requestedContextMenuPosition = globalPosition;
                         ++contextMenuRequestCount;
                     });
    const QPoint localContextMenuPosition = textLayer->viewport()->rect().center();
    const QPoint globalContextMenuPosition =
        textLayer->viewport()->mapToGlobal(localContextMenuPosition);
    QContextMenuEvent contextMenuEvent(QContextMenuEvent::Mouse, localContextMenuPosition,
                                       globalContextMenuPosition);
    QApplication::sendEvent(textLayer->viewport(), &contextMenuEvent);
    require(contextMenuRequestCount == 1 &&
                requestedContextMenuPosition == globalContextMenuPosition &&
                contextMenuEvent.isAccepted(),
            "embedded formatted text should route context menus to its owning pinned window");

    textItem->clearFocus();
    textLayer->clearFocus();
    QImage dpiImage(canvasSize.toSize(), QImage::Format_ARGB32_Premultiplied);
    dpiImage.setDevicePixelRatio(2.0);
    dpiImage.fill(Qt::white);
    QPainter dpiPainter(&dpiImage);
    textLayer->render(&dpiPainter, QRectF(QPointF(), QSizeF(textLayer->size())), textLayer->rect());
    dpiPainter.end();
    const QRect canonicalBounds = nonWhitePixelBounds(canonicalImage);
    const QRect dpiBounds = nonWhitePixelBounds(dpiImage);
    require(canonicalBounds.isValid() && dpiBounds.isValid() &&
                std::abs(canonicalBounds.left() - dpiBounds.left()) <= 1 &&
                std::abs(canonicalBounds.top() - dpiBounds.top()) <= 1 &&
                std::abs(canonicalBounds.width() - dpiBounds.width()) <= 2 &&
                std::abs(canonicalBounds.height() - dpiBounds.height()) <= 2,
            "a 2x backing image should reproduce the canonical HTML text size through the "
            "canvas transform");

    window.resize(180, 80);
    QApplication::processEvents();
    require(std::abs(textLayer->transform().m11() - 0.25) < 0.001 &&
                std::abs(textLayer->transform().m22() - 0.25) < 0.001,
            "formatted clipboard text should track subsequent canvas zoom changes");

    auto replacement = std::make_shared<QTextDocument>();
    replacement->setPlainText(QStringLiteral("Replacement formatted text"));
    window.showFormattedText(replacement);
    QApplication::processEvents();
    textLayer = window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    textItem = formattedTextItem(textLayer);
    require(textItem != nullptr && textItem->document() == replacement.get() &&
                qFuzzyCompare(textItem->scale(), 2.0),
            "replacing visible formatted text should detach the previous document first");

    window.clearFormattedText();
    require(window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText")) == nullptr,
            "leaving formatted-text mode should remove its selectable surface");
}

void qrContentsUseStrictRichTextLinksAndPreserveOrder() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();

    QList<QUrl> activatedLinks;
    ScreenshotRecognitionWindowActions actions;
    actions.handleLinkActivated =
        [&activatedLinks](const QUrl& url) { activatedLinks.push_back(url); };
    ScreenshotRecognitionWindow window(std::move(actions));
    const QRect geometry(screen->availableGeometry().center() - QPoint(240, 90),
                         QSize(480, 180));
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                geometry,
                QRectF(0.0, 0.0, 480.0, 180.0),
            }),
            "the QR recognition window should present a valid centered geometry");

    const QStringList contents{
        QStringLiteral("first <b>& payload"),
        QStringLiteral("https://example.com/path?x=1&y=2"),
        QStringLiteral("ftp://example.com/not-clickable"),
        QStringLiteral("prefix https://example.com/not-complete"),
        QStringLiteral("  http://qt.io/docs  "),
    };
    window.showQrContents(contents);
    QApplication::processEvents();

    auto* browser =
        window.findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents"));
    require(browser != nullptr && browser->isVisible() && browser->isReadOnly() &&
                !browser->openExternalLinks() && !browser->openLinks(),
            "QR contents should use a read-only browser with controller-owned navigation");
    require(browser->toPlainText() == contents.join(QLatin1Char('\n')),
            "QR payloads should be rendered as escaped text in recognition order");
    require(qobject_cast<adqt::widgets::AdScrollBar*>(browser->verticalScrollBar()) != nullptr,
            "QR contents should use the themed vertical scrollbar");

    QStringList anchorTexts;
    QStringList anchorHrefs;
    for (QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor()) {
                anchorTexts.push_back(fragment.text());
                anchorHrefs.push_back(fragment.charFormat().anchorHref());
            }
        }
    }
    require(anchorTexts == QStringList{QStringLiteral("https://example.com/path?x=1&y=2"),
                                       QStringLiteral("http://qt.io/docs")},
            "only complete HTTP and HTTPS payloads should become links");
    require(anchorHrefs.size() == 2 &&
                QUrl(anchorHrefs.at(0)).host() == QStringLiteral("example.com") &&
                QUrl(anchorHrefs.at(1)).host() == QStringLiteral("qt.io"),
            "recognized links should preserve valid absolute navigation targets");

    const QUrl clicked(QStringLiteral("https://example.com/path?x=1&y=2"));
    require(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, clicked)),
            "the QR browser should expose its anchor activation signal");
    require(activatedLinks == QList<QUrl>{clicked},
            "QR anchor activation should be forwarded exactly once to the controller");

    window.clearQrContents();
    require(window.findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents")) == nullptr,
            "leaving QR mode should remove its rich-text content surface");
    window.hide();
    QApplication::processEvents();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    embeddedRecognitionWindowPreservesParentSurfaceWithVisibleTextLayer();
    recognitionWindowCanExtendBeyondItsDpiScreen();
    recognitionWindowUsesOrdinaryQtWindowBehavior();
    shortRecognitionWindowPreservesExactSelectionGeometryAcrossModes();
    formattedClipboardTextUsesASelectableQtDocument();
    qrContentsUseStrictRichTextLinksAndPreserveOrder();
    return 0;
}
