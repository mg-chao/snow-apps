#include "snow_canvas_text_editor_connector.h"
#include "snow_canvas_text_editor_input.h"
#include "snow_canvas_text_editor_session.h"
#include "snow_canvas_text_editor_view.h"
#include "snow_canvas_cursor_controller.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_draft.h"
#include "snow_canvas_text_edit_target.h"
#include "snow_canvas_text_edit_geometry.h"
#include "snow_canvas_text_layout.h"
#include "snow_canvas_text_measurement.h"
#include "snow_canvas_type_conversions.h"
#include "snow_canvas_changed_viewports.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_viewport.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_widget_selection_hit_testing.h"
#include "snow_canvas_widget_pointer_flow.h"
#include "snow_canvas_widget_repaint.h"
#include "snow_canvas_state.h"
#include "snow_canvas_widget_text_interaction.h"
#include "demo_serial_number_controls.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template <typename T> const T& requireValue(const std::optional<T>& value, const char* message) {
    if (!value.has_value()) {
        std::cerr << message << '\n';
        std::exit(1);
    }
    return *value;
}

void requireNear(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 0.0001) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void replacementNormalizesLineBreaksAndSupportsUndoRedo() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("abc"));
    draft.setCursorPosition(1);
    draft.replaceSelection(QStringLiteral("x\r\ny"));

    require(draft.text() == QStringLiteral("ax\nybc"), "replacement should normalize CRLF");
    require(draft.cursorPosition() == 4, "cursor should move after normalized replacement");
    require(draft.undoEdit(), "undo should be available after replacement");
    require(draft.text() == QStringLiteral("abc"), "undo should restore original text");
    require(draft.cursorPosition() == 1, "undo should restore cursor");
    require(draft.redoEdit(), "redo should be available after undo");
    require(draft.text() == QStringLiteral("ax\nybc"), "redo should restore replacement");
}

SnowCanvasStyleToolbarState serialNumberToolbarState(SnowCanvasStyleToolbarSource source,
                                                     qint64 number, quint32 mixed = 0) {
    SnowCanvasStyleToolbarState state;
    state.source = source;
    state.serialNumberStyle.number = number;
    state.serialNumberStyleMixed = mixed;
    return state;
}

void demoSerialNumberControlsRespectSelectedMixedDecreaseState() {
    const SnowCanvasStyleToolbarState selectedZero =
        serialNumberToolbarState(SnowCanvasStyleToolbarSource::SelectedSerialNumber, 0);
    require(!demo_serial_number_controls::selectedSerialNumberCanDecrease(selectedZero),
            "selected serial controls should not decrease a concrete zero");

    const SnowCanvasStyleToolbarState selectedMixed =
        serialNumberToolbarState(SnowCanvasStyleToolbarSource::SelectedSerialNumber, 0,
                                 SnowCanvasSerialNumberStyleMixedNumber);
    require(demo_serial_number_controls::selectedSerialNumberCanDecrease(selectedMixed),
            "selected serial controls should allow decrease when mixed selection may include "
            "positive numbers");
    require(demo_serial_number_controls::serialNumberControlsCanDecrease(selectedMixed),
            "side-panel serial controls should inherit selected mixed decrease state");
}

void demoSerialNumberControlsClampDefaultDecrease() {
    const SnowCanvasStyleToolbarState defaultZero =
        serialNumberToolbarState(SnowCanvasStyleToolbarSource::DefaultSerialNumber, 0);
    require(!demo_serial_number_controls::serialNumberControlsCanDecrease(defaultZero),
            "default serial controls should not enable decrease at zero");

    require(demo_serial_number_controls::defaultSerialNumberAfterStep(3, -5) == 0,
            "default serial stepping should clamp below zero");
    require(demo_serial_number_controls::defaultSerialNumberAfterStep(3, 2) == 5,
            "default serial stepping should increase by the requested delta");
}

void cursorPositionReportsOnlyRealStateChanges() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("abc"));

    require(!draft.setCursorPosition(3), "setting the current cursor should not report a change");
    require(draft.setCursorPosition(1), "moving the cursor should report a change");
    require(!draft.hasSelection(), "plain cursor movement should clear selection");
    require(draft.setCursorPosition(2, true), "shift-style cursor movement should report a change");
    require(draft.selectionStart() == 1, "selection should keep the original anchor");
    require(draft.selectionEnd() == 2, "selection should extend to the new cursor");
    require(!draft.setCursorPosition(2, true),
            "repeating selected cursor position should be a no-op");
    require(draft.setCursorPosition(2), "dropping selection at the cursor should report a change");
    require(!draft.hasSelection(), "non-selecting movement should collapse selection");
}

void inputMethodPreeditDoesNotCommitUntilCommitString() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("ab"));
    draft.setCursorPosition(1);

    QInputMethodEvent preedit(QStringLiteral("中"), {});
    require(draft.handleInputMethodEvent(preedit), "preedit should change display text");
    require(draft.text() == QStringLiteral("ab"), "preedit should not mutate committed text");
    require(draft.displayText() == QStringLiteral("a中b"), "preedit should appear in display text");
    require(draft.hasPreedit(), "preedit state should be active");
    require(draft.inputMethodCursorPosition() == 1,
            "IME cursor should report committed-text anchor");

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("中"));
    require(draft.handleInputMethodEvent(commit), "commit should change committed text");
    require(draft.text() == QStringLiteral("a中b"), "commit should replace preedit range");
    require(!draft.hasPreedit(), "commit should clear preedit state");
    require(draft.cursorPosition() == 2, "commit should move cursor after committed text");
}

void inputMethodCommitReplacementUsesCursorRelativeRange() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("abcd"));
    draft.setCursorPosition(2);

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("XY"), -1, 2);
    require(draft.handleInputMethodEvent(commit), "IME replacement commit should change text");
    require(draft.text() == QStringLiteral("aXYd"),
            "IME replacement should use cursor-relative range");
    require(draft.cursorPosition() == 3,
            "IME replacement should leave cursor after committed text");
    require(draft.undoEdit(), "IME replacement should be undoable");
    require(draft.text() == QStringLiteral("abcd"),
            "undo should restore text before IME replacement");
    require(draft.cursorPosition() == 2, "undo should restore cursor before IME replacement");
}

void keyCommandsInsertTextAndReportEditorCommands() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("ab"));
    draft.setCursorPosition(1);

    QKeyEvent insertEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    const snow_canvas_text_editor_input::KeyResult insertResult =
        snow_canvas_text_editor_input::handleKeyPress(&insertEvent, draft, {});
    require(insertResult.handled, "printable key should be handled by text editor");
    require(insertResult.changed, "printable key should change the draft");
    require(insertResult.command == snow_canvas_text_editor_input::EventCommand::None,
            "printable key should not request editor lifecycle command");
    require(draft.text() == QStringLiteral("axb"), "printable key should insert into draft");

    QKeyEvent commitEvent(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    const snow_canvas_text_editor_input::KeyResult commitResult =
        snow_canvas_text_editor_input::handleKeyPress(&commitEvent, draft, {});
    require(commitResult.handled, "control-enter should be handled");
    require(!commitResult.changed, "control-enter should not mutate draft text");
    require(commitResult.command == snow_canvas_text_editor_input::EventCommand::Commit,
            "control-enter should request commit");

    QKeyEvent cancelEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    const snow_canvas_text_editor_input::KeyResult cancelResult =
        snow_canvas_text_editor_input::handleKeyPress(&cancelEvent, draft, {});
    require(cancelResult.handled, "escape should be handled");
    require(!cancelResult.changed, "escape should not mutate draft text");
    require(cancelResult.command == snow_canvas_text_editor_input::EventCommand::Cancel,
            "escape should request cancel");
}

void keyCommandsDelegateCursorMovement() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("abc"));

    QTextCursor::MoveOperation movedOperation = QTextCursor::NoMove;
    QTextCursor::MoveMode movedMode = QTextCursor::MoveAnchor;
    QKeyEvent leftEvent(QEvent::KeyPress, Qt::Key_Left, Qt::ShiftModifier);
    const snow_canvas_text_editor_input::KeyResult result =
        snow_canvas_text_editor_input::handleKeyPress(
            &leftEvent, draft,
            [&movedOperation, &movedMode](QTextCursor::MoveOperation operation,
                                          QTextCursor::MoveMode mode) {
                movedOperation = operation;
                movedMode = mode;
                return true;
            });

    require(result.handled, "left arrow should be handled");
    require(result.changed, "left arrow should request editor refresh");
    require(movedOperation == QTextCursor::PreviousCharacter,
            "left arrow should request previous-character movement");
    require(movedMode == QTextCursor::KeepAnchor, "shift-left should keep anchor while moving");
}

void keyCommandsReportNoChangeForNoopEdits() {
    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("abc"));
    draft.setCursorPosition(0);

    QKeyEvent backspaceEvent(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    const snow_canvas_text_editor_input::KeyResult backspaceResult =
        snow_canvas_text_editor_input::handleKeyPress(&backspaceEvent, draft, {});
    require(backspaceResult.handled, "backspace at start should still be handled");
    require(!backspaceResult.changed, "backspace at start should not report a text change");
    require(draft.text() == QStringLiteral("abc"), "backspace at start should preserve text");

    QKeyEvent leftEvent(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    const snow_canvas_text_editor_input::KeyResult leftResult =
        snow_canvas_text_editor_input::handleKeyPress(
            &leftEvent, draft, [&draft](QTextCursor::MoveOperation, QTextCursor::MoveMode mode) {
                return draft.setCursorPosition(0, mode == QTextCursor::KeepAnchor);
            });
    require(leftResult.handled, "left at start should still be handled");
    require(!leftResult.changed, "left at start should not report a cursor change");
    require(draft.cursorPosition() == 0, "left at start should preserve cursor");
}

void finishedEditCommitPolicyRequiresViewportAndContent() {
    SnowCanvasTextEditorSession::FinishedEdit edit;

    require(!edit.shouldCommit(true), "new empty finished text edit should not commit");

    edit.text = QStringLiteral("   ");
    require(!edit.shouldCommit(true), "new whitespace-only finished text edit should not commit");

    edit.text = QStringLiteral("text");
    require(edit.shouldCommit(true),
            "new non-empty finished text edit should commit when a viewport exists");
    require(!edit.shouldCommit(false), "finished text edit should not commit without a viewport");

    edit.text.clear();
    edit.hasExistingElement = true;
    require(edit.shouldCommit(true), "existing finished text edit should commit even when emptied");
    require(!edit.shouldCommit(false),
            "existing finished text edit should still require a viewport");
}

void textLayoutMapsDocumentRectsToLocalItemRects() {
    snow_canvas_text_layout::DocumentLayout layout;
    layout.itemWidth = 200.0;
    layout.itemHeight = 100.0;
    layout.topOffset = 10.0;
    layout.resolution.scale = 2.0;
    layout.document = std::make_unique<QTextDocument>();
    layout.document->setPlainText(QStringLiteral("abc"));

    const QRectF localRect =
        snow_canvas_text_layout::documentRectToLocalItemRect(QRectF(3.0, 4.0, 5.0, 6.0), layout);
    requireNear(localRect.left(), -94.0, "document rect should map x into local item coordinates");
    requireNear(localRect.top(), -32.0, "document rect should map y into local item coordinates");
    requireNear(localRect.width(), 10.0, "document rect width should scale to item coordinates");
    requireNear(localRect.height(), 12.0, "document rect height should scale to item coordinates");

    const QRectF contents = snow_canvas_text_layout::documentContentsRect(layout);
    requireNear(contents.left(), 0.0, "document contents should start at document origin");
    requireNear(contents.width(), 100.0,
                "document contents width should be unscaled document width");
    require(contents.height() > 0.0, "document contents should expose document height");
}

void resizedAlignedTextKeepsCaretClickAndSelectionGeometryConsistent() {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    item.center_x = 0.0;
    item.center_y = 0.0;
    item.width = 4000.0;
    item.height = 160.0;
    item.font_size = snow_canvas_text::defaultTextFontSize();
    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_CENTER;
    item.text_vertical_align = SNOW_TEXT_VERTICAL_ALIGN_CENTER;

    const QString text = QStringLiteral(
        "resizing this text element to a narrower width must keep every wrapped line editable");
    snow_canvas_text_layout::DocumentLayout wideLayout =
        snow_canvas_text_layout::createDocumentLayout(item, QFont(), 1.0, text, true);
    QTextBlock wideBlock = wideLayout.textDocument().begin();
    require(wideBlock.isValid() && wideBlock.layout() != nullptr &&
                wideBlock.layout()->lineCount() == 1,
            "text should begin on one line before its width is reduced");

    item.width = 140.0;
    snow_canvas_text_layout::DocumentLayout layout =
        snow_canvas_text_layout::createDocumentLayout(item, QFont(), 1.0, text, true);
    QTextBlock block = layout.textDocument().begin();
    require(block.isValid() && block.layout() != nullptr,
            "resized aligned text should create a laid-out text block");
    require(block.layout()->lineCount() > 1, "reducing text width should create wrapped lines");
    const QTextLine line = block.layout()->lineAt(1);
    require(line.isValid() && line.textStart() > 0 && line.textLength() > 1,
            "resized aligned text should create a non-empty second line");

    const QRectF blockRect = layout.textDocument().documentLayout()->blockBoundingRect(block);
    const int blockCursorPosition = line.textStart() + 1;
    const int documentCursorPosition = block.position() + blockCursorPosition;
    const qreal firstCursorX = line.cursorToX(blockCursorPosition);
    const qreal secondCursorX = line.cursorToX(blockCursorPosition + 1);
    const QRectF caret = snow_canvas_text_layout::cursorRectInDocument(layout.textDocument(),
                                                                       documentCursorPosition);
    requireNear(caret.left(), blockRect.left() + firstCursorX,
                "resized wrapped caret should use a block-relative QTextLine cursor position");

    const QVector<QRectF> selectionRects = snow_canvas_text_layout::rangeRectsInDocument(
        layout.textDocument(), documentCursorPosition, documentCursorPosition + 1);
    require(selectionRects.size() == 1,
            "single-character resized aligned selection should create one rectangle");
    requireNear(selectionRects[0].left(), blockRect.left() + qMin(firstCursorX, secondCursorX),
                "resized wrapped selection should use block-relative QTextLine cursor positions");
    requireNear(selectionRects[0].width(), qMax<qreal>(1.0, std::abs(secondCursorX - firstCursorX)),
                "resized aligned selection width should match the selected glyph advance");

    SceneDisplayInfo sceneInfo{};
    sceneInfo.surface_width = 800.0;
    sceneInfo.surface_height = 600.0;
    sceneInfo.camera_zoom = 1.0;
    const QRectF caretInView = snow_canvas_text_editor_view::cursorRect(
        item, QFont(), sceneInfo, text, documentCursorPosition);
    const int hitPosition = snow_canvas_text_editor_view::cursorPositionForViewPosition(
        item, QFont(), sceneInfo, caretInView.center(), text);
    require(hitPosition == documentCursorPosition,
            "clicking a resized aligned caret should resolve to the painted cursor position");
}

void textFontSizeFallbackIsSharedAcrossCreationAndMeasurement() {
    requireNear(snow_canvas_text::defaultTextFontSize(), 30.0,
                "default text font size should be 30px");
    requireNear(SnowCanvasTextStyle{}.fontSize, snow_canvas_text::defaultTextFontSize(),
                "Qt text style should use the shared default font size");
    require(SnowCanvasTextStyle{}.color == QColor(0xf4, 0x21, 0x2c),
            "Qt text style should use the default text color");
    require(SnowCanvasSerialNumberStyle{}.color == QColor(0xf4, 0x21, 0x2c),
            "Qt serial-number style should use the default serial-number color");
    requireNear(SnowCanvasTextStyle{}.cornerRadii.topLeft, 6.0,
                "Qt text style should use the default corner radius");

    SnowTextStyle zeroStyle{};
    zeroStyle.font_size = 0.0;
    SnowTextStyle defaultStyle{};
    defaultStyle.font_size = snow_canvas_text::defaultTextFontSize();

    requireNear(snow_canvas_text::resolvedTextFontSize(zeroStyle.font_size),
                snow_canvas_text::defaultTextFontSize(),
                "non-positive text font size should resolve to default");
    requireNear(snow_canvas_text::resolvedTextFontSize(1.0),
                snow_canvas_text::minimumTextFontSize(),
                "positive text font size below engine minimum should clamp to minimum");

    SnowCanvasSceneItem item;
    snow_canvas_text::applyTextStyleToSceneItem(item, zeroStyle);
    requireNear(item.font_size, snow_canvas_text::defaultTextFontSize(),
                "style application should clamp non-positive font size to default");

    const QFont baseFont;
    const SnowTextElementInfo info =
        snow_canvas_text::newTextInfoAt(QPointF(10.0, 20.0), baseFont, zeroStyle);
    requireNear(info.font_size, snow_canvas_text::defaultTextFontSize(),
                "new text info should use default font size");

    const SnowTextLayoutSize zeroLayout =
        snow_canvas_text_measurement::measureEmptyDraftLayout(zeroStyle, baseFont);
    const SnowTextLayoutSize defaultLayout =
        snow_canvas_text_measurement::measureEmptyDraftLayout(defaultStyle, baseFont);
    requireNear(zeroLayout.width, defaultLayout.width,
                "empty draft width should use shared default font size");
    requireNear(zeroLayout.height, defaultLayout.height,
                "empty draft height should use shared default font size");
}

void serialNumberBoundTextMeasurementUsesCreatedTextStyle() {
    const QFont baseFont;
    SnowTextStyle textStyle{};
    textStyle.font_size = 12.0;
    SnowSerialNumberStyle serialNumberStyle{};
    serialNumberStyle.font_size = 42.0;

    SnowTextStyle createdTextStyle = textStyle;
    createdTextStyle.font_size = serialNumberStyle.font_size;
    const SnowTextLayoutSize expectedLayout =
        snow_canvas_text_measurement::measureEmptyDraftLayout(createdTextStyle, baseFont);
    const SnowTextLayoutSize actualLayout =
        snow_canvas_text_measurement::measureSerialNumberBoundTextLayout(
            textStyle, serialNumberStyle, baseFont);

    requireNear(actualLayout.width, expectedLayout.width,
                "serial bound text width should use the created text style");
    requireNear(actualLayout.height, expectedLayout.height,
                "serial bound text height should use the created text style");
    require(actualLayout.height >
                snow_canvas_text_measurement::measureEmptyDraftLayout(textStyle, baseFont).height,
            "serial bound text measurement should not use the default text font size");
}

void textElementInfoDecodingBuildsOwnedPreview() {
    SnowTextElementInfo info{};
    const QByteArray text = QByteArrayLiteral("abc");
    std::memcpy(info.text_utf8, text.constData(), static_cast<std::size_t>(text.size()));
    info.text_utf8_len = static_cast<std::uint32_t>(text.size());
    info.text_truncated = 1;

    const QByteArray family = QByteArrayLiteral("Inter");
    std::memcpy(info.font_family_utf8, family.constData(), static_cast<std::size_t>(family.size()));
    info.font_family_utf8_len = static_cast<std::uint32_t>(family.size());
    info.font_family_truncated = 1;

    require(snow_canvas_text::textFromElementInfo(info) == QStringLiteral("abc"),
            "text element info should decode bounded UTF-8 text");

    const SnowCanvasSceneItem item = snow_canvas_text::defaultPreviewItem(info);
    require(snow_canvas_text::textFromSceneItem(item) == QStringLiteral("abc"),
            "preview item should carry decoded element text");
    require(snow_canvas_text::fontFamilyFromSceneItem(item) == QStringLiteral("Inter"),
            "preview item should carry decoded font family");
}

void textSceneItemCopyPreservesCompleteUtf8() {
    const QString emoji = QString::fromUtf8("\xf0\x9f\x98\x80");

    const int textPrefixLength = SNOW_TEXT_UTF8_CAPACITY - 1;
    require(textPrefixLength > 0, "text UTF-8 capacity should allow truncation boundary test");
    const QString textPrefix(textPrefixLength, QLatin1Char('a'));
    SnowCanvasSceneItem textItem;
    snow_canvas_text::copyTextToSceneItem(textItem, textPrefix + emoji);

    require(textItem.text_utf8_len ==
                static_cast<std::uint32_t>((textPrefix + emoji).toUtf8().size()),
            "owned scene item text should preserve complete UTF-8 content");
    require(snow_canvas_text::textFromSceneItem(textItem) == textPrefix + emoji,
            "owned scene item text should decode the complete value");
}

void textStyleConversionsTruncateFontFamilyAtUtf8Boundaries() {
    const QString emoji = QString::fromUtf8("\xf0\x9f\x98\x80");
    const int prefixLength = SNOW_FONT_FAMILY_UTF8_CAPACITY - 1;
    require(prefixLength > 0, "font-family UTF-8 capacity should allow truncation boundary test");
    const QString prefix(prefixLength, QLatin1Char('f'));

    SnowCanvasTextStyle textStyle;
    textStyle.fontFamily = prefix + emoji;
    const SnowTextStyle engineTextStyle = snow_canvas_types::toEngineTextStyle(textStyle);
    require(engineTextStyle.font_family_utf8_len == static_cast<std::uint32_t>(prefixLength),
            "text style conversion should avoid partial UTF-8 code points");
    require(engineTextStyle.font_family_truncated != 0,
            "text style conversion should mark UTF-8 boundary truncation");
    require(snow_canvas_types::toCanvasTextStyle(engineTextStyle).fontFamily == prefix,
            "text style conversion should round-trip the valid truncated font-family prefix");

    SnowCanvasSerialNumberStyle serialStyle;
    serialStyle.fontFamily = prefix + emoji;
    const SnowSerialNumberStyle engineSerialStyle =
        snow_canvas_types::toEngineSerialNumberStyle(serialStyle);
    require(engineSerialStyle.font_family_utf8_len == static_cast<std::uint32_t>(prefixLength),
            "serial-number style conversion should avoid partial UTF-8 code points");
    require(engineSerialStyle.font_family_truncated != 0,
            "serial-number style conversion should mark UTF-8 boundary truncation");
    require(
        snow_canvas_types::toCanvasSerialNumberStyle(engineSerialStyle).fontFamily == prefix,
        "serial-number style conversion should round-trip the valid truncated font-family prefix");
}

void textStyleStateEqualityBoundsFontFamilyLengths() {
    SnowTextStyle lhs{};
    SnowTextStyle rhs{};
    lhs.font_family_truncated = 1;
    rhs.font_family_truncated = 1;
    lhs.font_family_utf8_len = SNOW_FONT_FAMILY_UTF8_CAPACITY + 1;
    rhs.font_family_utf8_len = SNOW_FONT_FAMILY_UTF8_CAPACITY + 8;
    std::memset(lhs.font_family_utf8, 'a', sizeof(lhs.font_family_utf8));
    std::memset(rhs.font_family_utf8, 'a', sizeof(rhs.font_family_utf8));

    require(snow_canvas_state::textStylesEqual(lhs, rhs),
            "text style equality should compare only bounded font-family bytes");

    rhs.font_family_utf8[SNOW_FONT_FAMILY_UTF8_CAPACITY - 1] = 'b';
    require(!snow_canvas_state::textStylesEqual(lhs, rhs),
            "text style equality should still detect bounded font-family byte changes");

    SnowSerialNumberStyle serialLhs{};
    SnowSerialNumberStyle serialRhs{};
    serialLhs.font_family_truncated = 1;
    serialRhs.font_family_truncated = 1;
    serialLhs.font_family_utf8_len = SNOW_FONT_FAMILY_UTF8_CAPACITY + 3;
    serialRhs.font_family_utf8_len = SNOW_FONT_FAMILY_UTF8_CAPACITY + 5;
    std::memset(serialLhs.font_family_utf8, 's', sizeof(serialLhs.font_family_utf8));
    std::memset(serialRhs.font_family_utf8, 's', sizeof(serialRhs.font_family_utf8));

    require(snow_canvas_state::serialNumberStylesEqual(serialLhs, serialRhs),
            "serial-number style equality should compare only bounded font-family bytes");
}

void textMeasurementBuildsAutoResizeLayoutOverridesFromSnapshots() {
    SnowTextStyle style{};
    style.font_size = snow_canvas_text::defaultTextFontSize();

    SnowTextElementInfo infos[2] = {};
    infos[0].id = SnowElementId{10, 2};
    infos[0].font_size = style.font_size;
    infos[0].auto_resize = 1;
    const QByteArray measuredText = QByteArrayLiteral("auto");
    std::memcpy(infos[0].text_utf8, measuredText.constData(),
                static_cast<std::size_t>(measuredText.size()));
    infos[0].text_utf8_len = static_cast<std::uint32_t>(measuredText.size());

    infos[1].id = SnowElementId{11, 2};
    infos[1].font_size = style.font_size;
    infos[1].auto_resize = 0;

    const snow_canvas_text_measurement::TextLayoutOverrideMeasurement measurement =
        snow_canvas_text_measurement::measureAutoResizeLayoutOverrides(infos, 2, style, QFont());

    require(measurement.success, "snapshot layout measurement should succeed");
    require(measurement.layouts.size() == 1,
            "snapshot layout measurement should skip fixed-size text");
    require(measurement.layouts[0].id.index == infos[0].id.index &&
                measurement.layouts[0].id.generation == infos[0].id.generation,
            "snapshot layout measurement should preserve measured text id");
    require(measurement.layouts[0].size.width > 1.0,
            "snapshot layout measurement should produce width");
    require(measurement.layouts[0].size.height > 1.0,
            "snapshot layout measurement should produce height");
}

void textEditTargetResolvesCreateHitAndSelectedText() {
    ScopedRuntimeHandle runtime;
    require(snow_runtime_create(runtime.outParam()) == SNOW_OK,
            "runtime creation should succeed for text edit target tests");

    SnowCanvasViewport viewport;
    SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(viewport.create(runtime.get(), config),
            "viewport creation should succeed for text edit target tests");

    QFont baseFont;
    SnowTextStyle style{};
    style.font_size = snow_canvas_text::defaultTextFontSize();

    const QPointF createPoint(12.0, 34.0);
    require(!snow_canvas_text_edit_target::resolveTextEditTarget(
                 runtime.get(), viewport.get(), createPoint, baseFont, style, false)
                 .has_value(),
            "text edit target should not create a draft when creation is disabled");

    const std::optional<SnowTextElementInfo> created =
        snow_canvas_text_edit_target::resolveTextEditTarget(runtime.get(), viewport.get(),
                                                            createPoint, baseFont, style, true);
    const SnowTextElementInfo& createdValue =
        requireValue(created, "text edit target should create a draft target on empty canvas");
    requireNear(createdValue.center_x, createPoint.x(), "created text edit target center x");
    requireNear(createdValue.center_y, createPoint.y(), "created text edit target center y");

    const QByteArray text = QByteArrayLiteral("target");
    require(snow_viewport_create_text(runtime.get(), viewport.get(), 50.0, 60.0, text.constData(),
                                      static_cast<std::uint32_t>(text.size()), 40.0,
                                      20.0) == SNOW_OK,
            "runtime should create text for edit target hit testing");

    const QPointF hitPoint(50.0, 60.0);
    const std::optional<SnowTextElementInfo> hit =
        snow_canvas_text_edit_target::resolveTextEditTarget(runtime.get(), viewport.get(), hitPoint,
                                                            baseFont, style, false);
    require(hit.has_value(), "text edit target should resolve existing hit text");
    require(snow_canvas_text::textFromElementInfo(*hit) == QStringLiteral("target"),
            "text edit target should return the hit text element info");

    SnowElementId hitId{};
    std::uint8_t hitFlag = 0;
    require(snow_viewport_hit_text(runtime.get(), viewport.get(), hitPoint.x(), hitPoint.y(),
                                   &hitId, &hitFlag) == SNOW_OK &&
                hitFlag != 0,
            "runtime should expose the created text id for selected target testing");

    ScopedChangedViewportList changedViewports;
    require(snow_viewport_select_element_ex(runtime.get(), viewport.get(), hitId,
                                            changedViewports.outParam()) == SNOW_OK,
            "runtime should select text for selected edit target testing");

    const std::optional<SnowTextElementInfo> selected =
        snow_canvas_text_edit_target::resolveSelectedTextEditTarget(runtime.get(), viewport.get(),
                                                                    hitPoint, false);
    require(selected.has_value(),
            "selected text edit target should resolve a selected hit text element");
    require(snow_canvas_text::textFromElementInfo(*selected) == QStringLiteral("target"),
            "selected text edit target should return selected text element info");

    require(!snow_canvas_text_edit_target::resolveSelectedTextEditTarget(
                 runtime.get(), viewport.get(), hitPoint, true)
                 .has_value(),
            "serial-bound selected target should reject ordinary selected text");
}

void textEditGeometryAnchorsRespectHorizontalAlignment() {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    item.center_x = 100.0;
    item.center_y = 50.0;
    item.width = 40.0;
    item.height = 20.0;
    item.rotation = 0.0;

    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    QPointF anchor = snow_canvas_text_edit_geometry::topAnchorForItem(item);
    requireNear(anchor.x(), 80.0, "left-aligned text top anchor x");
    requireNear(anchor.y(), 40.0, "left-aligned text top anchor y");
    QPointF center = snow_canvas_text_edit_geometry::centerForTopAnchor(item, anchor);
    requireNear(center.x(), item.center_x, "left-aligned top anchor should round-trip center x");
    requireNear(center.y(), item.center_y, "left-aligned top anchor should round-trip center y");

    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_CENTER;
    anchor = snow_canvas_text_edit_geometry::topAnchorForItem(item);
    requireNear(anchor.x(), 100.0, "center-aligned text top anchor x");
    requireNear(anchor.y(), 40.0, "center-aligned text top anchor y");

    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_RIGHT;
    anchor = snow_canvas_text_edit_geometry::topAnchorForItem(item);
    requireNear(anchor.x(), 120.0, "right-aligned text top anchor x");
    requireNear(anchor.y(), 40.0, "right-aligned text top anchor y");

    const QPointF creationAnchor =
        snow_canvas_text_edit_geometry::topAnchorForCreationPoint(item, QPointF(10.0, 30.0));
    requireNear(creationAnchor.x(), 10.0, "new text creation anchor x");
    requireNear(creationAnchor.y(), 20.0, "new text creation anchor y");
}

void textEditorViewMapsCanvasHitTestingAndEditingRegion() {
    SceneDisplayInfo sceneInfo{};
    sceneInfo.surface_width = 200.0;
    sceneInfo.surface_height = 100.0;
    sceneInfo.camera_center_x = 10.0;
    sceneInfo.camera_center_y = 20.0;
    sceneInfo.camera_zoom = 2.0;

    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    item.element_id = SnowElementId{5, 2};
    item.center_x = 15.0;
    item.center_y = 25.0;
    item.width = 20.0;
    item.height = 10.0;
    item.font_size = snow_canvas_text::defaultTextFontSize();
    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    item.text_vertical_align = SNOW_TEXT_VERTICAL_ALIGN_CENTER;

    const QPointF centerView =
        snow_canvas_text_editor_view::canvasToViewPoint(sceneInfo, QPointF(15.0, 25.0));
    requireNear(centerView.x(), 110.0, "text editor view should map canvas x through scene camera");
    requireNear(centerView.y(), 60.0, "text editor view should map canvas y through scene camera");

    require(snow_canvas_text_editor_view::itemContainsViewPosition(item, sceneInfo, centerView),
            "text editor view hit testing should include the item center");
    require(!snow_canvas_text_editor_view::itemContainsViewPosition(item, sceneInfo,
                                                                    QPointF(131.0, 60.0)),
            "text editor view hit testing should reject points outside the item bounds");

    SnowCanvasTextDraft draft;
    draft.begin(QStringLiteral("abc"));
    const QRegion region =
        snow_canvas_text_editor_view::editingRegion(item, draft, QFont(), sceneInfo);
    require(!region.isEmpty(), "text editor view editing region should be non-empty");
    require(region.contains(centerView.toPoint()),
            "text editor view editing region should include preview bounds");

    const QRectF logicalBounds = snow_canvas_render_geometry::sceneItemBounds(sceneInfo, item);
    item.fill = SnowColorRgba8{0xff, 0xff, 0xff, 0xff};
    const QRectF fillBounds = snow_canvas_render_geometry::sceneItemBounds(sceneInfo, item);
    require(
        fillBounds.left() < logicalBounds.left() && fillBounds.right() > logicalBounds.right() &&
            fillBounds.top() < logicalBounds.top() && fillBounds.bottom() > logicalBounds.bottom(),
        "editing text bounds should include background fill padding");
    const QRegion fillEditingRegion =
        snow_canvas_text_editor_view::editingRegion(item, draft, QFont(), sceneInfo);
    require(
        fillEditingRegion.contains(snow_canvas_render_geometry::alignedRectForBounds(fillBounds)),
        "text editing dirty region should cover the background fill");

    item.fill = SnowColorRgba8{};
    item.stroke = SnowColorRgba8{0, 0, 0, 0xff};
    item.stroke_width = 12.0;
    item.text_utf8_len = 3;
    const QRectF strokeBounds = snow_canvas_render_geometry::sceneItemBounds(sceneInfo, item);
    require(strokeBounds.left() < logicalBounds.left() &&
                strokeBounds.right() > logicalBounds.right() &&
                strokeBounds.top() < logicalBounds.top() &&
                strokeBounds.bottom() > logicalBounds.bottom(),
            "editing text bounds should include visible stroke paint");
    const QRegion strokeEditingRegion =
        snow_canvas_text_editor_view::editingRegion(item, draft, QFont(), sceneInfo);
    require(strokeEditingRegion.contains(
                snow_canvas_render_geometry::alignedRectForBounds(strokeBounds)),
            "text editing dirty region should cover the visible stroke");

    const QRectF caret = snow_canvas_text_editor_view::cursorRect(
        item, QFont(), sceneInfo, draft.displayText(), draft.displayCursorPosition());
    require(!caret.isEmpty(), "text editor view should resolve a caret rectangle");

    const int cursorPosition = snow_canvas_text_editor_view::cursorPositionForViewPosition(
        item, QFont(), sceneInfo, centerView, draft.text());
    require(cursorPosition >= 0 && cursorPosition <= draft.text().size(),
            "text editor view should clamp hit-tested cursor positions to draft text bounds");
}

void bahnschriftCondensedMeasurementUsesDocumentHeight() {
    const QString family = QStringLiteral("Bahnschrift Condensed");
    SnowTextElementInfo info{};
    info.font_size = 64.0;
    SnowCanvasSceneItem item = snow_canvas_text::defaultPreviewItem(info);
    item.setFontFamilyUtf8(family.toUtf8());

    const QFont baseFont = QApplication::font();
    const snow_canvas_text_layout::FontResolution font =
        snow_canvas_text_layout::resolveFont(baseFont, item, 1.0);
    if (!QFontInfo(font.font).family().contains(QStringLiteral("Bahnschrift"))) {
        return;
    }

    const QString text = QStringLiteral("Bahnschrift Condensed");
    const QSizeF measuredSize = snow_canvas_text_layout::measureNaturalText(text, baseFont, item);
    item.width = measuredSize.width();
    item.height = measuredSize.height();
    snow_canvas_text_layout::DocumentLayout documentLayout =
        snow_canvas_text_layout::createDocumentLayout(item, baseFont, 1.0, text, false);
    const QTextBlock block = documentLayout.textDocument().begin();
    require(block.isValid() && block.layout() != nullptr && block.layout()->lineCount() == 1,
            "Bahnschrift Condensed natural measurement should produce one document line");
    requireNear(measuredSize.height(),
                documentLayout.textDocument().size().height() * documentLayout.resolution.scale,
                "Bahnschrift Condensed measurement should use the painted document line height");
}

QRect darkPixelBounds(const QImage& image) {
    QRect bounds;
    bool hasDarkPixel = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 0 || pixel.red() >= 250 || pixel.green() >= 250 ||
                pixel.blue() >= 250) {
                continue;
            }
            const QRect pixelRect(x, y, 1, 1);
            bounds = hasDarkPixel ? bounds.united(pixelRect) : pixelRect;
            hasDarkPixel = true;
        }
    }
    return bounds;
}

void bahnschriftCondensedSerialNumberUsesResolvedGlyphBounds() {
    constexpr double zoom = 1.37;
    const QString family = QStringLiteral("Bahnschrift Condensed");
    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
    item.width = 40.0;
    item.height = 40.0;
    item.font_size = 63.25;
    item.serial_number = 88;
    item.fill = SnowColorRgba8{0xff, 0xff, 0xff, 0xff};
    item.stroke = SnowColorRgba8{0, 0, 0, 0};
    item.text_color = SnowColorRgba8{0, 0, 0, 0xff};
    item.fill_style = SNOW_FILL_STYLE_SOLID;
    item.opacity = 1.0;
    item.setFontFamilyUtf8(family.toUtf8());

    const QFont baseFont = QApplication::font();
    snow_canvas_text_layout::SingleLineLayout layout =
        snow_canvas_text_layout::createSingleLineLayout(QStringLiteral("88"), baseFont, item, zoom);
    if (!QFontInfo(layout.resolution.font).family().contains(QStringLiteral("Bahnschrift"))) {
        return;
    }
    require(layout.visualBounds.isValid() && !layout.visualBounds.isEmpty(),
            "Bahnschrift Condensed serial number should have resolved glyph bounds");
    requireNear(layout.resolution.font.pixelSize() * layout.resolution.scale, item.font_size * zoom,
                "serial-number font resolution should preserve the requested view pixel size");

    const QSize imageSize(320, 320);
    SceneDisplayInfo sceneInfo{};
    sceneInfo.surface_width = imageSize.width();
    sceneInfo.surface_height = imageSize.height();
    sceneInfo.camera_zoom = zoom;
    QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setFont(baseFont);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &sceneInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    const QRect paintedBounds = darkPixelBounds(image);
    require(!paintedBounds.isEmpty(), "Bahnschrift Condensed serial number should paint text");
    const QSizeF resolvedVisualSize = layout.visualBounds.size() * layout.resolution.scale;
    const double contentSize = item.width * zoom;
    const double fitScale = qMin(1.0, qMin(contentSize / resolvedVisualSize.width(),
                                           contentSize / resolvedVisualSize.height()));
    const QSizeF measuredViewSize = resolvedVisualSize * fitScale;
    require(std::abs(paintedBounds.width() - measuredViewSize.width()) <= 2.0,
            "serial-number painted width should match resolved glyph width");
    require(std::abs(paintedBounds.height() - measuredViewSize.height()) <= 2.0,
            "serial-number painted height should match resolved glyph height");
    require(std::abs(paintedBounds.center().x() - image.rect().center().x()) <= 1.0,
            "serial-number measured glyphs should be horizontally centered");
    require(std::abs(paintedBounds.center().y() - image.rect().center().y()) <= 1.0,
            "serial-number measured glyphs should be vertically centered");
}

bool textEditorOverlayHasVisiblePixels(SnowCanvasWidgetTextInteraction& interaction,
                                       const SnowCanvasDisplayCache& displayCache,
                                       const QFont& font, const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    interaction.renderEditorOverlay(painter, font, displayCache);
    painter.end();

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                return true;
            }
        }
    }
    return false;
}

bool waitForTextEditorOverlayVisibility(SnowCanvasWidgetTextInteraction& interaction,
                                        const SnowCanvasDisplayCache& displayCache,
                                        const QFont& font, const QSize& size, bool expectedVisible,
                                        int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (textEditorOverlayHasVisiblePixels(interaction, displayCache, font, size) ==
            expectedVisible) {
            return true;
        }
        QThread::msleep(1);
    } while (elapsed.elapsed() < timeoutMs);
    return false;
}

bool textEditorOverlayRemainsVisible(SnowCanvasWidgetTextInteraction& interaction,
                                     const SnowCanvasDisplayCache& displayCache, const QFont& font,
                                     const QSize& size, int durationMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < durationMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!textEditorOverlayHasVisiblePixels(interaction, displayCache, font, size)) {
            return false;
        }
        QThread::msleep(1);
    }
    return true;
}

void textEditorCaretBlinksResetsAndHonorsSystemFlashTime() {
    constexpr int kFlashTimeMs = 80;
    constexpr int kVisibilityTimeoutMs = 1000;
    const int originalFlashTimeMs = QApplication::cursorFlashTime();
    QApplication::setCursorFlashTime(kFlashTimeMs);

    ScopedRuntimeHandle runtime;
    require(snow_runtime_create(runtime.outParam()) == SNOW_OK,
            "runtime creation should succeed for text caret blink tests");
    SnowCanvasViewport viewport;
    require(viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()),
            "viewport creation should succeed for text caret blink tests");

    const QSize surfaceSize(200, 160);
    require(snow_viewport_set_surface_size(runtime.get(), viewport.get(), surfaceSize.width(),
                                           surfaceSize.height()) == SNOW_OK,
            "text caret blink viewport should accept its surface size");
    SnowCanvasDisplayCache displayCache;
    require(displayCache.sync(runtime.get(), viewport.get()),
            "text caret blink test should synchronize the display cache");

    QWidget editorHost;
    editorHost.resize(surfaceSize);
    editorHost.show();
    QApplication::processEvents();

    SnowTextStyle style{};
    style.color = SnowColorRgba8{0x1e, 0x1e, 0x1e, 0xff};
    style.font_size = snow_canvas_text::defaultTextFontSize();
    style.opacity = 1.0;
    style.horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    style.vertical_align = SNOW_TEXT_VERTICAL_ALIGN_CENTER;
    const SnowTextElementInfo info =
        snow_canvas_text::newTextInfoAt(QPointF(0.0, 0.0), editorHost.font(), style);

    SnowCanvasCursorController cursorController(editorHost);
    SnowCanvasWidgetTextInteraction interaction(editorHost, cursorController);
    require(interaction.beginForElement(
                info, displayCache, QPointF(surfaceSize.width() / 2.0, surfaceSize.height() / 2.0),
                &style, false),
            "text caret blink test should begin an empty text edit");
    require(textEditorOverlayHasVisiblePixels(interaction, displayCache, editorHost.font(),
                                              surfaceSize),
            "a newly activated text caret should start visible");
    require(waitForTextEditorOverlayVisibility(interaction, displayCache, editorHost.font(),
                                               surfaceSize, false, kVisibilityTimeoutMs),
            "an active custom-painted text caret should enter its hidden blink phase");

    const QRegion resetRegion = interaction.setCursorFromViewPosition(
        displayCache, editorHost.font(),
        QPointF(surfaceSize.width() / 2.0, surfaceSize.height() / 2.0), false);
    require(!resetRegion.isEmpty(),
            "placing the cursor at the same position should invalidate a hidden caret");
    require(textEditorOverlayHasVisiblePixels(interaction, displayCache, editorHost.font(),
                                              surfaceSize),
            "cursor interaction should immediately restart the visible blink phase");

    require(waitForTextEditorOverlayVisibility(interaction, displayCache, editorHost.font(),
                                               surfaceSize, false, kVisibilityTimeoutMs),
            "a restarted caret should continue blinking after its visible phase");
    QApplication::setCursorFlashTime(0);
    QApplication::processEvents();
    require(textEditorOverlayHasVisiblePixels(interaction, displayCache, editorHost.font(),
                                              surfaceSize),
            "disabling system cursor flashing should immediately reveal a hidden caret");
    require(textEditorOverlayRemainsVisible(interaction, displayCache, editorHost.font(),
                                            surfaceSize, kFlashTimeMs * 2),
            "a zero system cursor flash time should keep the caret steadily visible");

    const SnowCanvasWidgetTextInteraction::CancelResult cancelResult =
        interaction.cancel(runtime.get(), viewport.get(), displayCache);
    require(cancelResult.sessionEnded, "cancel should end the blinking text edit session");
    require(!textEditorOverlayHasVisiblePixels(interaction, displayCache, editorHost.font(),
                                               surfaceSize),
            "ending text editing should remove the caret overlay");

    QApplication::setCursorFlashTime(originalFlashTimeMs);
}

SnowOverlayDisplayItem overlayRect(SnowOverlayRectKind kind, double centerX, double centerY,
                                   double width, double height) {
    SnowOverlayDisplayItem item{};
    item.kind = SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT;
    item.rect_kind = kind;
    item.center_x = centerX;
    item.center_y = centerY;
    item.width = width;
    item.height = height;
    return item;
}

snow_canvas_render_geometry::ViewProjection testProjection() {
    return snow_canvas_render_geometry::ViewProjection{
        0.0, 0.0, 1.0, 200.0, 200.0,
    };
}

bool isTextSelectionControl(const SnowOverlayDisplayItem& item) {
    if (item.kind != SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT) {
        return false;
    }
    return item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_FRAME ||
           item.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME ||
           item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_RESIZE_HANDLE ||
           item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_ROTATION_HANDLE;
}

std::vector<SnowOverlayDisplayItem>
textSelectionControls(const SnowCanvasDisplayCache& displayCache) {
    std::vector<SnowOverlayDisplayItem> controls;
    for (std::uint32_t index = 0; index < displayCache.overlayItemCount(); ++index) {
        const SnowOverlayDisplayItem& item = displayCache.overlayItems()[index];
        if (isTextSelectionControl(item)) {
            controls.push_back(item);
        }
    }
    return controls;
}

bool sameColor(const SnowColorRgba8& left, const SnowColorRgba8& right) {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

void syncDisplayCacheForChangedViewport(SnowCanvasDisplayCache& displayCache, SnowRuntime runtime,
                                        const SnowCanvasViewport& viewport,
                                        SnowChangedViewportList changedViewports,
                                        const char* failureMessage) {
    std::uint64_t viewportId = 0;
    require(viewport.id(&viewportId), "changed viewport test should resolve its viewport id");
    const snow_canvas_changed_viewports::ViewportIds changedIds =
        snow_canvas_changed_viewports::idsFromList(changedViewports);
    for (std::uint64_t changedId : changedIds) {
        if (changedId == viewportId) {
            require(displayCache.sync(runtime, viewport.get()), failureMessage);
            return;
        }
    }
}

bool imageRegionContainsSelectionBlue(const QImage& image, const QRect& region) {
    const QRect clipped = region.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.blue() >= 224 && pixel.blue() - pixel.red() >= 40 &&
                pixel.blue() - pixel.green() >= 20 && pixel.alpha() > 0) {
                return true;
            }
        }
    }
    return false;
}

QImage renderCanvasWidget(SnowCanvasWidget& canvas) {
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas.render(&painter);
    painter.end();
    return image;
}

void sendCanvasMouseEvent(SnowCanvasWidget& canvas, QEvent::Type type, const QPointF& position,
                          Qt::MouseButton button, Qt::MouseButtons buttons,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, position, position, button, buttons, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

void beginTextDraftWithContent(SnowCanvasWidget& canvas) {
    require(canvas.setCanvasTool(SnowCanvasTool::Text),
            "text focus test should activate the text tool");
    canvas.setFocus(Qt::OtherFocusReason);
    sendCanvasMouseEvent(canvas, QEvent::MouseButtonPress, QPointF(80.0, 80.0), Qt::LeftButton,
                         Qt::LeftButton);
    sendCanvasMouseEvent(canvas, QEvent::MouseButtonRelease, QPointF(80.0, 80.0), Qt::LeftButton,
                         Qt::NoButton);
    QKeyEvent insertEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QApplication::sendEvent(&canvas, &insertEvent);
}

bool canvasReportsInputMethodEnabled(SnowCanvasWidget& canvas) {
    QInputMethodQueryEvent event(Qt::ImEnabled);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImEnabled).toBool();
}

void inputMethodEnablementTracksInlineTextEditing() {
    SnowCanvasWidget canvas;
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();

    require(!canvas.testAttribute(Qt::WA_InputMethodEnabled),
            "an idle canvas must not advertise input-method support");
    require(!canvasReportsInputMethodEnabled(canvas),
            "an idle canvas must report input methods disabled");
    require(canvas.setCanvasTool(SnowCanvasTool::Text),
            "input-method lifecycle test should activate the text tool");
    require(!canvas.testAttribute(Qt::WA_InputMethodEnabled),
            "selecting the text tool must not enable input methods before editing");

    beginTextDraftWithContent(canvas);
    require(canvas.testAttribute(Qt::WA_InputMethodEnabled),
            "an active inline text draft must enable input methods");
    require(canvasReportsInputMethodEnabled(canvas),
            "an active inline text draft must report input methods enabled");
    require(canvas.cancelActiveTextEditing(),
            "input-method lifecycle test should cancel its first text draft");
    require(!canvas.testAttribute(Qt::WA_InputMethodEnabled),
            "canceling inline text editing must disable input methods");

    beginTextDraftWithContent(canvas);
    require(canvas.setCanvasTool(SnowCanvasTool::Select),
            "input-method lifecycle test should commit by changing tools");
    require(!canvas.testAttribute(Qt::WA_InputMethodEnabled),
            "committing inline text editing must disable input methods");
    require(!canvasReportsInputMethodEnabled(canvas),
            "a committed canvas must report input methods disabled");
}

void textEditorStylePopupInteractionPreservesDraftUntilItCloses() {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto* canvas = new SnowCanvasWidget(&host);
    auto* unrelatedControl = new QLineEdit(&host);
    layout->addWidget(canvas);
    layout->addWidget(unrelatedControl);
    host.resize(400, 240);
    host.show();

    QWidget stylePopupSurface;
    stylePopupSurface.setWindowFlags(Qt::Tool);
    auto* stylePopupControl = new QLineEdit(&stylePopupSurface);
    stylePopupSurface.resize(160, 32);
    stylePopupSurface.show();
    QApplication::processEvents();

    beginTextDraftWithContent(*canvas);
    require(!canvas->canvasHistoryState().canUndo,
            "an active text draft should remain outside history before it is committed");

    canvas->beginTextStylePopupInteraction();
    stylePopupControl->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    require(!canvas->canvasHistoryState().canUndo,
            "an explicit text-style popup interaction should keep the draft active");

    stylePopupSurface.hide();
    host.activateWindow();
    canvas->endTextStylePopupInteraction(&host);
    QApplication::processEvents();
    require(canvas->hasFocus(), "canvas should regain focus after the style popup closes");
    unrelatedControl->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    require(canvas->canvasHistoryState().canUndo,
            "moving focus to an unrelated control should commit the active text draft");
}

void cancelingAnActiveTextDraftDoesNotCommitIt() {
    SnowCanvasWidget canvas;
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();

    beginTextDraftWithContent(canvas);
    require(!canvas.canvasHistoryState().canUndo,
            "an active text draft should remain outside history before cancellation");
    require(canvas.cancelActiveTextEditing(),
            "canceling an active text draft should report a state change");
    require(canvas.setCanvasTool(SnowCanvasTool::Select),
            "switching tools after cancellation should remain supported");
    require(!canvas.canvasHistoryState().canUndo,
            "canceling an active text draft must not commit it to history");
}

SnowInputEvent pointerInput(SnowPointerEventType type, double x, double y, SnowPointerButton button,
                            std::uint8_t buttons) {
    SnowInputEvent event{};
    event.kind = SNOW_INPUT_EVENT_POINTER;
    event.pointer.pointer_id = 1;
    event.pointer.event_type = type;
    event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
    event.pointer.position_x = x;
    event.pointer.position_y = y;
    event.pointer.button = button;
    event.pointer.buttons = buttons;
    return event;
}

void selectToolDragRendersSelectionMarquee() {
    SnowCanvasRuntime runtime;
    require(runtime.isValid(), "selection marquee runtime should be valid");
    const SnowRuntime runtimeHandle = snow_canvas_runtime::Access::handle(runtime);

    SnowCanvasViewport viewport;
    require(viewport.create(runtimeHandle, snow_canvas_viewport::defaultEngineConfig()),
            "selection marquee viewport should be created");
    require(snow_viewport_set_surface_size(runtimeHandle, viewport.get(), 320, 240) == SNOW_OK,
            "selection marquee viewport should accept its surface size");
    ScopedChangedViewportList toolChange;
    require(snow_viewport_set_active_tool_ex(runtimeHandle, viewport.get(),
                                             SNOW_ACTIVE_TOOL_SELECT,
                                             toolChange.outParam()) == SNOW_OK,
            "selection marquee test should activate select tool");

    SnowInteractionOutput output{};
    SnowInputEvent press =
        pointerInput(SNOW_POINTER_EVENT_DOWN, 60.0, 70.0, SNOW_POINTER_BUTTON_PRIMARY, 0b0000'0001);
    ScopedChangedViewportList pressChange;
    require(snow_viewport_process_input_ex(runtimeHandle, viewport.get(), &press, &output,
                                           pressChange.outParam()) == SNOW_OK,
            "selection marquee press should reach the engine");

    SnowInputEvent move =
        pointerInput(SNOW_POINTER_EVENT_MOVE, 260.0, 190.0, SNOW_POINTER_BUTTON_NONE, 0b0000'0001);
    ScopedChangedViewportList moveChange;
    require(snow_viewport_process_input_ex(runtimeHandle, viewport.get(), &move, &output,
                                           moveChange.outParam()) == SNOW_OK,
            "selection marquee move should reach the engine");

    SnowCanvasDisplayCache displayCache;
    require(displayCache.sync(runtimeHandle, viewport.get()),
            "selection marquee overlay should synchronize to the display cache");

    const SnowOverlayDisplayItem* marquee = nullptr;
    for (std::uint32_t index = 0; index < displayCache.overlayItemCount(); ++index) {
        const SnowOverlayDisplayItem& item = displayCache.overlayItems()[index];
        if (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT &&
            item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_MARQUEE) {
            marquee = &item;
            break;
        }
    }
    require(marquee != nullptr, "select-tool drag should expose a selection marquee overlay");
    requireNear(marquee->width, 200.0, "selection marquee width");
    requireNear(marquee->height, 120.0, "selection marquee height");
    require(sameColor(marquee->fill, SnowColorRgba8{0x40, 0x96, 0xff, 0x33}),
            "selection marquee fill should match the reference blue at 20 percent opacity");
    require(sameColor(marquee->stroke, SnowColorRgba8{0x40, 0x96, 0xff, 0xff}),
            "selection marquee stroke should match the reference blue");
    requireNear(marquee->stroke_width, 1.0, "selection marquee stroke width");

    QImage image(QSize(320, 240), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderOverlayItems(
        painter, displayCache.overlayInfo(), displayCache.overlayItems(),
        displayCache.overlayItemCount(), QRegion(image.rect()));
    painter.end();

    const QColor fillPixel = image.pixelColor(160, 130);
    require(fillPixel.red() < 230 && fillPixel.blue() > fillPixel.red(),
            "selection marquee should paint its translucent fill");
    require(imageRegionContainsSelectionBlue(image, QRect(56, 66, 9, 9)),
            "selection marquee should paint its blue outline");
}

bool isElementSelected(SnowRuntime runtime, SnowViewport viewport, SnowElementId elementId) {
    std::uint8_t selected = 0;
    require(snow_viewport_is_element_selected(runtime, viewport, elementId, &selected) == SNOW_OK,
            "selection interaction test should query the selected element");
    return selected != 0;
}

void requireSameTextSelectionControls(const std::vector<SnowOverlayDisplayItem>& actual,
                                      const std::vector<SnowOverlayDisplayItem>& expected) {
    require(actual.size() == expected.size(),
            "text selection control count should match select tool");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const SnowOverlayDisplayItem& actualItem = actual[index];
        const SnowOverlayDisplayItem& expectedItem = expected[index];
        require(actualItem.kind == expectedItem.kind,
                "text selection control kind should match select tool");
        require(actualItem.rect_kind == expectedItem.rect_kind,
                "text selection rectangle kind should match select tool");
        requireNear(actualItem.center_x, expectedItem.center_x,
                    "text selection center x should match select tool");
        requireNear(actualItem.center_y, expectedItem.center_y,
                    "text selection center y should match select tool");
        requireNear(actualItem.width, expectedItem.width,
                    "text selection width should match select tool");
        requireNear(actualItem.height, expectedItem.height,
                    "text selection height should match select tool");
        requireNear(actualItem.rotation, expectedItem.rotation,
                    "text selection rotation should match select tool");
        require(sameColor(actualItem.fill, expectedItem.fill),
                "text selection fill should match select tool");
        require(sameColor(actualItem.stroke, expectedItem.stroke),
                "text selection stroke should match select tool");
        requireNear(actualItem.stroke_width, expectedItem.stroke_width,
                    "text selection stroke width should match select tool");
        require(actualItem.fill_style == expectedItem.fill_style,
                "text selection fill style should match select tool");
        requireNear(actualItem.corner_radii.top_left, expectedItem.corner_radii.top_left,
                    "text selection top-left radius should match select tool");
        requireNear(actualItem.corner_radii.top_right, expectedItem.corner_radii.top_right,
                    "text selection top-right radius should match select tool");
        requireNear(actualItem.corner_radii.bottom_right, expectedItem.corner_radii.bottom_right,
                    "text selection bottom-right radius should match select tool");
        requireNear(actualItem.corner_radii.bottom_left, expectedItem.corner_radii.bottom_left,
                    "text selection bottom-left radius should match select tool");
    }
}

void selectionHitTestingUsesMinimumHandleHitSize() {
    const SnowOverlayDisplayItem items[] = {
        overlayRect(SNOW_OVERLAY_RECT_SELECTION_RESIZE_HANDLE, 0.0, 0.0, 8.0, 8.0),
    };

    require(snow_canvas_widget_selection_hit_testing::pointerHitsSelectionInteractionItems(
                items, 1, testProjection(), QPointF(105.0, 100.0)),
            "selection handle hit testing should expand small handles");
    require(!snow_canvas_widget_selection_hit_testing::pointerHitsSelectionInteractionItems(
                items, 1, testProjection(), QPointF(107.0, 100.0)),
            "selection handle hit testing should not expand beyond minimum hit size");
}

void selectionHitTestingTreatsTextFramePaddingAsMoveRing() {
    const SnowOverlayDisplayItem items[] = {
        overlayRect(SNOW_OVERLAY_RECT_SELECTION_FRAME, 0.0, 0.0, 68.0, 48.0),
        overlayRect(SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME, 0.0, 0.0, 40.0, 20.0),
    };

    require(snow_canvas_widget_selection_hit_testing::pointerHitsSelectionInteractionItems(
                items, 2, testProjection(), QPointF(100.0, 78.0)),
            "text selection padded frame should hit in the move ring");
    require(!snow_canvas_widget_selection_hit_testing::pointerHitsSelectionInteractionItems(
                items, 2, testProjection(), QPointF(100.0, 100.0)),
            "text selection move ring should not consume the actual text frame");
}

void textEditorActivationPreservesSelectToolSelectionBox() {
    ScopedRuntimeHandle runtime;
    require(snow_runtime_create(runtime.outParam()) == SNOW_OK,
            "runtime creation should succeed for selection box consistency");

    SnowCanvasViewport viewport;
    const SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(viewport.create(runtime.get(), config),
            "viewport creation should succeed for selection box consistency");
    require(snow_viewport_set_surface_size(runtime.get(), viewport.get(), 600, 400) == SNOW_OK,
            "selection box consistency viewport should accept its surface size");

    const QByteArray text = QByteArrayLiteral("selection reference");
    require(snow_viewport_create_text(runtime.get(), viewport.get(), 0.0, 0.0, text.constData(),
                                      static_cast<std::uint32_t>(text.size()), 260.0,
                                      84.0) == SNOW_OK,
            "selection box consistency test should create a text element");
    ScopedChangedViewportList selectToolChange;
    require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                             SNOW_ACTIVE_TOOL_SELECT,
                                             selectToolChange.outParam()) == SNOW_OK,
            "selection box consistency test should activate the select tool");

    const std::optional<SnowTextElementInfo> target =
        snow_canvas_text_edit_target::resolveTextEditTarget(
            runtime.get(), viewport.get(), QPointF(0.0, 0.0), QFont(), SnowTextStyle{}, false);
    const SnowTextElementInfo& targetValue =
        requireValue(target, "selection box consistency test should resolve the text element");
    require(
        snow_canvas_commands::selectElement(runtime.get(), viewport.get(), targetValue.id).success,
        "selection box consistency test should select the text element");

    SnowCanvasDisplayCache displayCache;
    require(displayCache.sync(runtime.get(), viewport.get()),
            "selection box consistency test should synchronize the select-tool frame");
    const std::vector<SnowOverlayDisplayItem> selectControls = textSelectionControls(displayCache);
    require(selectControls.size() == 7, "select tool should render all text selection controls");

    ScopedChangedViewportList textToolChange;
    require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(), SNOW_ACTIVE_TOOL_TEXT,
                                             textToolChange.outParam()) == SNOW_OK,
            "selection box consistency test should activate the text tool");
    require(displayCache.sync(runtime.get(), viewport.get()),
            "selection box consistency test should synchronize the text tool");

    QWidget editorHost;
    SnowCanvasCursorController cursorController(editorHost);
    SnowCanvasWidgetTextInteraction interaction(editorHost, cursorController);
    const SnowCanvasWidgetTextInteraction::BeginResult beginResult = interaction.beginAt(
        runtime.get(), viewport.get(), displayCache, QPointF(300.0, 200.0), SnowTextStyle{}, false);
    require(beginResult.started, "selection box consistency test should activate text editing");
    syncDisplayCacheForChangedViewport(
        displayCache, runtime.get(), viewport, beginResult.firstChangedViewports.get(),
        "selection box consistency test should synchronize the selection mutation");
    syncDisplayCacheForChangedViewport(
        displayCache, runtime.get(), viewport, beginResult.secondChangedViewports.get(),
        "selection box consistency test should synchronize the draft mutation");
    const std::vector<SnowOverlayDisplayItem> editingControls = textSelectionControls(displayCache);
    requireSameTextSelectionControls(editingControls, selectControls);

    const snow_canvas_render_geometry::ViewProjection projection =
        snow_canvas_render_geometry::overlayProjection(displayCache.overlayInfo());
    const SnowOverlayDisplayItem* selectionFrame = nullptr;
    const SnowOverlayDisplayItem* textActualFrame = nullptr;
    for (const SnowOverlayDisplayItem& control : editingControls) {
        if (control.rect_kind == SNOW_OVERLAY_RECT_SELECTION_FRAME) {
            selectionFrame = &control;
        } else if (control.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME) {
            textActualFrame = &control;
        }
        if (control.rect_kind == SNOW_OVERLAY_RECT_SELECTION_RESIZE_HANDLE ||
            control.rect_kind == SNOW_OVERLAY_RECT_SELECTION_ROTATION_HANDLE) {
            require(interaction.selectionInteractionContains(
                        displayCache, snow_canvas_render_geometry::canvasToView(
                                          projection, control.center_x, control.center_y)),
                    "all canonical text selection handles should be interactive while editing");
        }
    }
    require(selectionFrame != nullptr && textActualFrame != nullptr,
            "text editing should expose both canonical selection frames");
    const QPointF frameCenter(selectionFrame->center_x, selectionFrame->center_y);
    const QPointF frameEdgeCanvas = snow_canvas_render_geometry::rotatePoint(
        QPointF(selectionFrame->center_x - selectionFrame->width / 2.0, selectionFrame->center_y),
        frameCenter, selectionFrame->rotation);
    require(interaction.selectionInteractionContains(
                displayCache, snow_canvas_render_geometry::canvasToView(
                                  projection, frameEdgeCanvas.x(), frameEdgeCanvas.y())),
            "the canonical text selection frame edge should be interactive while editing");
    const QPointF moveRingCanvas = snow_canvas_render_geometry::rotatePoint(
        QPointF(selectionFrame->center_x - (selectionFrame->width + textActualFrame->width) / 4.0,
                selectionFrame->center_y),
        frameCenter, selectionFrame->rotation);
    require(interaction.selectionInteractionContains(
                displayCache, snow_canvas_render_geometry::canvasToView(
                                  projection, moveRingCanvas.x(), moveRingCanvas.y())),
            "the canonical text padding move ring should be interactive while editing");
    require(!interaction.selectionInteractionContains(
                displayCache,
                snow_canvas_render_geometry::canvasToView(projection, textActualFrame->center_x,
                                                          textActualFrame->center_y)),
            "the text interior should remain available for caret placement while editing");
}

void newTextDraftClearsPreviouslySelectedText() {
    ScopedRuntimeHandle runtime;
    require(snow_runtime_create(runtime.outParam()) == SNOW_OK,
            "new text selection test should create a runtime");

    SnowCanvasViewport viewport;
    const SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(viewport.create(runtime.get(), config),
            "new text selection test should create a viewport");
    require(snow_viewport_set_surface_size(runtime.get(), viewport.get(), 600, 400) == SNOW_OK,
            "new text selection test should set the surface size");

    const QByteArray text = QByteArrayLiteral("previous text");
    require(snow_viewport_create_text(runtime.get(), viewport.get(), 0.0, 0.0, text.constData(),
                                      static_cast<std::uint32_t>(text.size()), 160.0,
                                      50.0) == SNOW_OK,
            "new text selection test should create the previous text");

    SnowElementId textId{};
    std::uint8_t hit = 0;
    require(snow_viewport_hit_text(runtime.get(), viewport.get(), 0.0, 0.0, &textId, &hit) ==
                    SNOW_OK &&
                hit != 0,
            "new text selection test should resolve the previous text");
    require(snow_canvas_commands::selectElement(runtime.get(), viewport.get(), textId).success,
            "new text selection test should select the previous text");
    require(isElementSelected(runtime.get(), viewport.get(), textId),
            "previous text should start selected");

    SnowCanvasDisplayCache displayCache;
    require(displayCache.sync(runtime.get(), viewport.get()),
            "new text selection test should synchronize the selected text");

    QWidget editorHost;
    SnowCanvasCursorController cursorController(editorHost);
    SnowCanvasWidgetTextInteraction interaction(editorHost, cursorController);
    const SnowCanvasWidgetTextInteraction::BeginResult beginResult = interaction.beginAt(
        runtime.get(), viewport.get(), displayCache, QPointF(500.0, 300.0), SnowTextStyle{}, true);
    require(beginResult.started, "blank canvas press should begin a new text draft");
    require(!isElementSelected(runtime.get(), viewport.get(), textId),
            "beginning a new text draft should clear the previously selected text");
}

void textToolInitialSelectionFrameRendersAndResizesThroughWidgetEvents() {
    SnowCanvasRuntime runtime;
    require(runtime.isValid(), "widget selection interaction runtime should be valid");
    SnowRuntime runtimeHandle = snow_canvas_runtime::Access::handle(runtime);

    SnowCanvasViewport inspectionViewport;
    const SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(inspectionViewport.create(runtimeHandle, config),
            "widget selection interaction viewport should be created");
    require(snow_viewport_set_surface_size(runtimeHandle, inspectionViewport.get(), 600, 400) ==
                SNOW_OK,
            "widget selection interaction viewport should accept its surface size");

    const QByteArray text = QByteArrayLiteral("interactive selection");
    require(snow_viewport_create_text(runtimeHandle, inspectionViewport.get(), 0.0, 0.0,
                                      text.constData(), static_cast<std::uint32_t>(text.size()),
                                      220.0, 64.0) == SNOW_OK,
            "widget selection interaction test should create text");
    const std::optional<SnowTextElementInfo> target =
        snow_canvas_text_edit_target::resolveTextEditTarget(runtimeHandle, inspectionViewport.get(),
                                                            QPointF(0.0, 0.0), QFont(),
                                                            SnowTextStyle{}, false);
    const SnowElementId targetId =
        requireValue(target, "widget selection interaction test should resolve its text").id;

    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0),
            "widget selection interaction camera should be configured");
    require(canvas.setCanvasTool(SnowCanvasTool::Text),
            "widget selection interaction test should activate the text tool");

    const QPointF textCenter(300.0, 200.0);
    sendCanvasMouseEvent(canvas, QEvent::MouseButtonPress, textCenter, Qt::LeftButton,
                         Qt::LeftButton);
    sendCanvasMouseEvent(canvas, QEvent::MouseButtonRelease, textCenter, Qt::LeftButton,
                         Qt::NoButton);
    QApplication::processEvents();
    require(isElementSelected(runtimeHandle, inspectionViewport.get(), targetId),
            "the first text-tool click should select the text before any content change");

    SnowCanvasDisplayCache inspectionCache;
    require(inspectionCache.sync(runtimeHandle, inspectionViewport.get()),
            "widget selection interaction test should inspect the canonical engine overlay");
    const std::vector<SnowOverlayDisplayItem> controls = textSelectionControls(inspectionCache);
    require(controls.size() == 7, "text editing should expose all canonical selection controls");

    const SnowOverlayDisplayItem* resizeHandle = nullptr;
    for (const SnowOverlayDisplayItem& control : controls) {
        if (control.rect_kind == SNOW_OVERLAY_RECT_SELECTION_RESIZE_HANDLE) {
            resizeHandle = &control;
            break;
        }
    }
    require(resizeHandle != nullptr, "text editing should expose a resize handle");
    const QPointF resizeHandlePosition = snow_canvas_render_geometry::canvasToView(
        snow_canvas_render_geometry::overlayProjection(inspectionCache.overlayInfo()),
        resizeHandle->center_x, resizeHandle->center_y);
    const QRect handlePaintRegion(static_cast<int>(std::floor(resizeHandlePosition.x())) - 6,
                                  static_cast<int>(std::floor(resizeHandlePosition.y())) - 6, 13,
                                  13);
    require(imageRegionContainsSelectionBlue(renderCanvasWidget(canvas), handlePaintRegion),
            "the initial text-tool selection resize handle should be rendered by the widget");

    SnowTextElementInfo beforeResize{};
    SnowTextStyle beforeStyle{};
    std::uint8_t activeDraft = 0;
    require(snow_viewport_get_active_text_draft_presentation(
                runtimeHandle, inspectionViewport.get(), &beforeResize, &beforeStyle,
                &activeDraft) == SNOW_OK &&
                activeDraft != 0,
            "widget selection interaction test should expose the active text draft");

    sendCanvasMouseEvent(canvas, QEvent::MouseButtonPress, resizeHandlePosition, Qt::LeftButton,
                         Qt::LeftButton);
    require(isElementSelected(runtimeHandle, inspectionViewport.get(), targetId),
            "pressing an editing-mode resize handle should not deselect the text");
    const QPointF resizedPosition = resizeHandlePosition - QPointF(24.0, 16.0);
    sendCanvasMouseEvent(canvas, QEvent::MouseMove, resizedPosition, Qt::NoButton, Qt::LeftButton);
    sendCanvasMouseEvent(canvas, QEvent::MouseButtonRelease, resizedPosition, Qt::LeftButton,
                         Qt::NoButton);
    QApplication::processEvents();

    SnowTextElementInfo afterResize{};
    SnowTextStyle afterStyle{};
    activeDraft = 0;
    require(snow_viewport_get_active_text_draft_presentation(
                runtimeHandle, inspectionViewport.get(), &afterResize, &afterStyle, &activeDraft) ==
                    SNOW_OK &&
                activeDraft != 0,
            "resizing should keep the text draft active");
    require(std::abs(afterResize.width - beforeResize.width) > 0.0001 ||
                std::abs(afterResize.height - beforeResize.height) > 0.0001 ||
                std::abs(afterResize.center_x - beforeResize.center_x) > 0.0001 ||
                std::abs(afterResize.center_y - beforeResize.center_y) > 0.0001,
            "dragging the editing-mode resize handle should transform the active text draft");
}

void textEditorDoesNotSynthesizeSelectionControlsWithoutEngineOverlay() {
    ScopedRuntimeHandle runtime;
    require(snow_runtime_create(runtime.outParam()) == SNOW_OK,
            "runtime creation should succeed for canonical text selection controls");

    SnowCanvasViewport viewport;
    const SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(viewport.create(runtime.get(), config),
            "viewport creation should succeed for canonical text selection controls");
    require(snow_viewport_set_surface_size(runtime.get(), viewport.get(), 200, 200) == SNOW_OK,
            "canonical text selection viewport should accept its surface size");

    const QByteArray text = QByteArrayLiteral("selected");
    require(snow_viewport_create_text(runtime.get(), viewport.get(), 0.0, 0.0, text.constData(),
                                      static_cast<std::uint32_t>(text.size()), 40.0,
                                      20.0) == SNOW_OK,
            "canonical text selection test should create a text element");
    ScopedChangedViewportList textToolChange;
    require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(), SNOW_ACTIVE_TOOL_TEXT,
                                             textToolChange.outParam()) == SNOW_OK,
            "canonical text selection test should activate the text tool");

    SnowCanvasDisplayCache displayCache;
    require(displayCache.sync(runtime.get(), viewport.get()),
            "canonical text selection test should synchronize the display cache");
    bool hasEngineTextSelectionFrame = false;
    for (std::uint32_t index = 0; index < displayCache.overlayItemCount(); ++index) {
        const SnowOverlayDisplayItem& item = displayCache.overlayItems()[index];
        hasEngineTextSelectionFrame =
            hasEngineTextSelectionFrame || (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT &&
                                            item.rect_kind == SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME);
    }
    require(!hasEngineTextSelectionFrame,
            "canonical text selection test requires a cache without an engine selection frame");

    const std::optional<SnowTextElementInfo> target =
        snow_canvas_text_edit_target::resolveTextEditTarget(
            runtime.get(), viewport.get(), QPointF(0.0, 0.0), QFont(), SnowTextStyle{}, false);
    require(target.has_value(), "canonical text selection test should resolve the created text");

    QWidget editorHost;
    SnowCanvasCursorController cursorController(editorHost);
    SnowCanvasWidgetTextInteraction interaction(editorHost, cursorController);
    require(
        interaction.beginForElement(*target, displayCache, QPointF(100.0, 100.0), nullptr, false),
        "canonical text selection test should begin an existing text edit");
    require(!interaction.selectionInteractionContains(displayCache, QPointF(66.0, 76.0)),
            "text editing should not hit-test controls absent from the engine overlay");

    QImage image(QSize(200, 200), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    interaction.renderEditorOverlay(painter, QFont(), displayCache);
    painter.end();

    require(!imageRegionContainsSelectionBlue(image, image.rect()),
            "text editing should not paint selection controls absent from the engine overlay");
}

int renderedOverlayFillPixelCount(SnowFillStyle fillStyle, double strokeWidth = 0.0) {
    QImage image(QSize(120, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowOverlayDisplayItem item{};
    item.kind = SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT;
    item.rect_kind = SNOW_OVERLAY_RECT_SELECTION_CANDIDATE_FRAME;
    item.width = 80.0;
    item.height = 60.0;
    item.fill = SnowColorRgba8{255, 0, 0, 255};
    item.fill_style = fillStyle;
    item.stroke_width = strokeWidth;

    OverlayDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderOverlayItems(painter, displayInfo, &item, 1, QRegion(image.rect()));
    painter.end();

    int filledPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++filledPixels;
            }
        }
    }
    return filledPixels;
}

int renderedSceneRectangleFillPixelCount(const SnowCornerRadii& cornerRadii) {
    QImage image(QSize(120, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowSceneDisplayItem borrowed{};
    borrowed.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    borrowed.width = 80.0;
    borrowed.height = 60.0;
    borrowed.fill = SnowColorRgba8{255, 0, 0, 255};
    borrowed.fill_style = SNOW_FILL_STYLE_LINE;
    borrowed.corner_radii = cornerRadii;
    borrowed.opacity = 1.0;
    SnowCanvasSceneItem item(borrowed);

    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &displayInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    int filledPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++filledPixels;
            }
        }
    }
    return filledPixels;
}

int renderedTextBackgroundPixelCount(SnowFillStyle fillStyle, double fontSize) {
    QImage image(QSize(160, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    item.width = 120.0;
    item.height = 80.0;
    item.fill = SnowColorRgba8{255, 0, 0, 255};
    item.fill_style = fillStyle;
    item.font_size = fontSize;
    item.opacity = 1.0;
    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    item.text_vertical_align = SNOW_TEXT_VERTICAL_ALIGN_CENTER;
    item.setTextUtf8(QByteArrayLiteral("Texture"));

    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &displayInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    int filledPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++filledPixels;
            }
        }
    }
    return filledPixels;
}

int renderedClosedLineFillPixelCount(SnowFillStyle fillStyle, bool closed) {
    QImage image(QSize(120, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const SnowArrowPoint points[] = {
        {20.0, 20.0},
        {100.0, 20.0},
        {60.0, 100.0},
        {closed ? 20.0 : 30.0, closed ? 20.0 : 90.0},
    };
    SnowArrowPathCommand commands[4]{};
    commands[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    commands[0].point = points[0];
    for (int index = 1; index < 4; ++index) {
        commands[index].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[index].point = points[index];
    }

    SnowSceneDisplayItem borrowed{};
    borrowed.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    borrowed.fill = SnowColorRgba8{255, 0, 0, 255};
    borrowed.fill_style = fillStyle;
    borrowed.stroke = SnowColorRgba8{0, 0, 0, 0};
    borrowed.stroke_width = 2.0;
    borrowed.arrow_point_count = 4;
    borrowed.arrow_points = points;
    borrowed.arrow_path_command_count = 4;
    borrowed.arrow_path_commands = commands;
    borrowed.arrow_type = SNOW_ARROW_TYPE_CURVE;
    borrowed.opacity = 1.0;
    SnowCanvasSceneItem item(borrowed);

    SnowPathChunk chunk{};
    chunk.stable_id = 1;
    chunk.command_count = 4;
    chunk.min_x = 20.0;
    chunk.min_y = 20.0;
    chunk.max_x = 100.0;
    chunk.max_y = 100.0;
    SnowPathChunkRange range{};
    range.insert_chunk_count = 1;
    require(item.applyPathGeometryPatch(0, 1, &range, 1, &chunk, 1, commands, 4, closed, true),
            "Line path geometry patch should be accepted");

    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &displayInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    int filledPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++filledPixels;
            }
        }
    }
    return filledPixels;
}

void closedLineRendererHonorsFillStyleAndExactClosure() {
    const int openPixels = renderedClosedLineFillPixelCount(SNOW_FILL_STYLE_SOLID, false);
    const int solidPixels = renderedClosedLineFillPixelCount(SNOW_FILL_STYLE_SOLID, true);
    const int linePixels = renderedClosedLineFillPixelCount(SNOW_FILL_STYLE_LINE, true);
    const int crossLinePixels = renderedClosedLineFillPixelCount(SNOW_FILL_STYLE_CROSS_LINE, true);
    require(openPixels == 0, "open Line paths must not render fill");
    require(solidPixels > 0, "closed Line paths should render solid fill");
    require(linePixels > 0, "closed Line paths should render line hatch fill");
    require(crossLinePixels > linePixels,
            "closed Line cross hatch should render both hatch directions");
}

void longOpenPathsRenderAllCommands() {
    QImage image(QSize(240, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    std::vector<SnowArrowPathCommand> commands(3001);
    commands[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    commands[0].point = SnowArrowPoint{0.0, 0.0};
    for (std::size_t index = 1; index < commands.size(); ++index) {
        commands[index].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[index].point = SnowArrowPoint{
            static_cast<double>(index) / 15.0,
            static_cast<double>(static_cast<int>(index % 5) - 2),
        };
    }
    const SnowArrowPoint endpoints[] = {{0.0, 0.0}, {200.0, 0.0}};
    SnowSceneDisplayItem borrowed{};
    borrowed.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    borrowed.stroke = SnowColorRgba8{255, 0, 0, 255};
    borrowed.stroke_width = 2.0;
    borrowed.arrow_point_count = 2;
    borrowed.arrow_points = endpoints;
    borrowed.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
    borrowed.arrow_path_commands = commands.data();
    borrowed.opacity = 1.0;
    SnowCanvasSceneItem item(borrowed);
    SceneDisplayInfo info{};
    info.item_count = 1;
    info.surface_width = image.width();
    info.surface_height = image.height();
    info.camera_center_x = 100.0;
    info.camera_zoom = 1.0;

    QPainter painter(&image);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(
        snow_canvas_renderer::SceneRenderRequest{&painter, &info, &item, 1, QRegion(image.rect())});
    painter.end();
    bool tailVisible = false;
    for (int y = 50; y < 70 && !tailVisible; ++y) {
        for (int x = 210; x < 230; ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                tailVisible = true;
                break;
            }
        }
    }
    require(tailVisible,
            "dynamic paths should render all path commands");
    QPainter cachedPainter(&image);
    cachedPainter.setClipRect(QRect(180, 0, 60, 120));
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &cachedPainter,
        &info,
        &item,
        1,
        QRegion(QRect(180, 0, 60, 120)),
    });
    cachedPainter.end();
}

int renderedSerialNumberBackgroundPixelCount(SnowFillStyle fillStyle, double fontSize) {
    QImage image(QSize(120, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
    item.width = 80.0;
    item.height = 80.0;
    item.fill = SnowColorRgba8{255, 0, 0, 255};
    item.fill_style = fillStyle;
    item.font_size = fontSize;
    item.opacity = 1.0;

    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &displayInfo, &item, 1, QRegion(image.rect())});
    painter.end();

    int filledPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++filledPixels;
            }
        }
    }
    return filledPixels;
}

void overlayRectangleRendererHonorsFillStyle() {
    const std::size_t cacheEntriesBefore =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int linePixels = renderedOverlayFillPixelCount(SNOW_FILL_STYLE_LINE);
    const std::size_t cacheEntriesAfterLine =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int crossLinePixels = renderedOverlayFillPixelCount(SNOW_FILL_STYLE_CROSS_LINE);
    const std::size_t cacheEntriesAfterCross =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int solidPixels = renderedOverlayFillPixelCount(SNOW_FILL_STYLE_SOLID);

    require(linePixels > 0, "line fill preview should render hatch pixels");
    require(crossLinePixels > linePixels,
            "cross-line fill preview should render both hatch directions");
    require(solidPixels > crossLinePixels,
            "non-solid fill previews should retain transparent gaps");
    require(cacheEntriesAfterLine <= cacheEntriesBefore + 1,
            "line fill should create at most one semantic hatch texture");
    require(cacheEntriesAfterCross == cacheEntriesAfterLine,
            "cross-line fill should reuse the line texture for its mirrored pass");
}

void textHoverUnderlineRendererDrawsOnlyTheUnderline() {
    QImage image(QSize(120, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowOverlayDisplayItem item =
        overlayRect(SNOW_OVERLAY_RECT_TEXT_HOVER_UNDERLINE, 0.0, 0.0, 80.0, 40.0);
    item.stroke = SnowColorRgba8{0x40, 0x96, 0xff, 0xff};
    item.stroke_width = 1.5;

    OverlayDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderOverlayItems(painter, displayInfo, &item, 1, QRegion(image.rect()));
    painter.end();

    require(imageRegionContainsSelectionBlue(image, QRect(18, 78, 84, 5)),
            "text hover feedback should render a blue underline");
    require(!imageRegionContainsSelectionBlue(image, QRect(18, 17, 84, 5)),
            "text hover feedback should not render a top selection-frame edge");
    require(!imageRegionContainsSelectionBlue(image, QRect(18, 19, 5, 38)),
            "text hover feedback should not render a side selection-frame edge");
}

void hatchTextureCacheReusesSaturatedStrokeWidths() {
    const std::size_t cacheEntriesBefore =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    require(renderedOverlayFillPixelCount(SNOW_FILL_STYLE_LINE, 10.0) > 0,
            "wide-stroke line fill should render hatch pixels");
    const std::size_t cacheEntriesAfterFirstWideStroke =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();

    for (double strokeWidth : {20.0, 100.0, 999.0}) {
        require(renderedOverlayFillPixelCount(SNOW_FILL_STYLE_LINE, strokeWidth) > 0,
                "saturated line fill should render hatch pixels");
    }
    const std::size_t cacheEntriesAfterAllWideStrokes =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();

    require(cacheEntriesAfterFirstWideStroke <= cacheEntriesBefore + 1,
            "the first saturated stroke width should create at most one hatch texture");
    require(cacheEntriesAfterAllWideStrokes == cacheEntriesAfterFirstWideStroke,
            "stroke widths above the hatch-spacing limit should share one cached texture");
}

void bindSerialToTextPreview(SnowSceneDisplayItem& serial, const SnowElementId& textId) {
    serial.bound_text_element_index = textId.index;
    serial.bound_text_element_generation = textId.generation;
    serial.has_bound_text_element = 1;
}

void textEditorConnectorBuildsSerialBoundConnector() {
    SnowSceneDisplayItem preview{};
    preview.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    preview.element_id = SnowElementId{42, 7};
    preview.center_x = 0.0;
    preview.center_y = -60.0;
    preview.width = 40.0;
    preview.height = 20.0;

    SnowSceneDisplayItem serial{};
    serial.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
    serial.element_id = SnowElementId{3, 11};
    serial.center_x = 0.0;
    serial.center_y = 0.0;
    serial.width = 20.0;
    serial.height = 20.0;
    serial.stroke_width = 2.0;
    serial.stroke = SnowColorRgba8{1, 2, 3, 255};
    serial.opacity = 0.75;
    bindSerialToTextPreview(serial, preview.element_id);

    SnowCanvasSceneItem connector;
    require(snow_canvas_text_editor_connector::itemBindsPreview(serial, &preview),
            "serial item should decode bound text preview id");
    require(snow_canvas_text_editor_connector::connectorItemForPreview(serial, preview, &connector),
            "serial-bound preview should create a connector item");
    require(connector.kind == SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR,
            "serial-bound preview connector should use connector scene item kind");
    require(connector.element_id.index == serial.element_id.index,
            "connector should keep serial id index");
    require(connector.element_id.generation == serial.element_id.generation,
            "connector should keep serial id generation");
    requireNear(connector.center_x, 0.0, "connector start x");
    requireNear(connector.center_y, -18.0, "connector start y");
    requireNear(connector.width, 0.0, "connector end x");
    requireNear(connector.height, -49.0, "connector end y");
    require(connector.arrow_point_count == 0,
            "centered connector should not add a baseline segment");
    requireNear(connector.stroke_width, 2.0, "connector should inherit serial stroke width");
    requireNear(connector.opacity, 0.75, "connector should inherit serial opacity");
}

void widgetPointerFlowPlansSuppressedTextCreate() {
    const snow_canvas_widget_pointer_flow::PressPlan plan =
        snow_canvas_widget_pointer_flow::planPress(snow_canvas_widget_pointer_flow::PressRequest{
            true,
            false,
            false,
            SnowCanvasTool::Text,
            Qt::LeftButton,
            Qt::NoModifier,
            false,
            true,
            false,
        });

    require(plan.shouldBeginText, "suppressed text press should still try to begin existing text");
    require(!plan.allowCreateText, "suppressed text press should disable new text creation");
    require(plan.suppressedTextCreateForPress,
            "suppressed text press should be recorded in the plan");
    require(plan.shouldAcceptSuppressedTextCreate,
            "suppressed text press should accept when no existing text begins");
}

void widgetPointerFlowDispatchesShiftClickInTextToolToEngine() {
    const snow_canvas_widget_pointer_flow::PressPlan plan =
        snow_canvas_widget_pointer_flow::planPress(snow_canvas_widget_pointer_flow::PressRequest{
            true,
            false,
            false,
            SnowCanvasTool::Text,
            Qt::LeftButton,
            Qt::ShiftModifier,
            false,
            false,
            false,
        });

    require(!plan.shouldBeginText,
            "shift-click in the text tool should not start inline text editing");
    require(plan.shouldDispatchToEngine,
            "shift-click in the text tool should reach engine selection handling");
}

void widgetPointerFlowDispatchesTextToolSelectionPressWithoutCommit() {
    const snow_canvas_widget_pointer_flow::PressPlan plan =
        snow_canvas_widget_pointer_flow::planPress(snow_canvas_widget_pointer_flow::PressRequest{
            true,
            true,
            false,
            SnowCanvasTool::Text,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            true,
            true,
        });

    require(!plan.shouldCommitTextEditor,
            "active text selection press should keep the draft active");
    require(plan.shouldDispatchToEngine,
            "text selection interaction press should dispatch to engine-owned selection geometry");
    require(!plan.shouldAcceptIfTextBeginFails,
            "text selection interaction press should not be consumed by editor commit");
    require(
        !plan.dispatchAfterCommitRequiresRestoredSelection,
        "selection press should not require restored selection because it does not commit first");
    require(plan.suppressedTextCreateRestoredSelection,
            "planner should carry restored selection from suppressed text create state");
}

void widgetPointerFlowDispatchesSelectToolSelectionPressWithoutCommit() {
    const snow_canvas_widget_pointer_flow::PressPlan plan =
        snow_canvas_widget_pointer_flow::planPress(snow_canvas_widget_pointer_flow::PressRequest{
            true,
            true,
            false,
            SnowCanvasTool::Select,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            false,
            false,
        });

    require(!plan.shouldCommitTextEditor,
            "select tool selection press should keep the active draft");
    require(plan.shouldDispatchToEngine, "select tool selection interaction press should dispatch "
                                         "to engine-owned selection geometry");
    require(!plan.shouldAcceptIfTextBeginFails,
            "select tool selection interaction press should not be consumed by editor commit");
    require(!plan.dispatchAfterCommitRequiresRestoredSelection,
            "select tool selection press should not require restored selection");
}

void widgetPointerFlowDispatchesAnyToolSelectionPressWithoutCommit() {
    const snow_canvas_widget_pointer_flow::PressPlan plan =
        snow_canvas_widget_pointer_flow::planPress(snow_canvas_widget_pointer_flow::PressRequest{
            true,
            true,
            false,
            SnowCanvasTool::Arrow,
            Qt::LeftButton,
            Qt::NoModifier,
            true,
            false,
            false,
        });

    require(!plan.shouldCommitTextEditor,
            "selection interaction press should keep the active draft");
    require(plan.shouldDispatchToEngine, "selection interaction press should dispatch to "
                                         "engine-owned selection geometry for any active tool");
    require(
        !plan.shouldAcceptIfTextBeginFails,
        "selection interaction press should not be consumed by editor commit for any active tool");
    require(
        !plan.dispatchAfterCommitRequiresRestoredSelection,
        "selection interaction press should not require restored selection for any active tool");
}

void textEditorWheelPlansFontSizeStepping() {
    const snow_canvas_text_editor_input::FontSizeWheelPlan angleWheel =
        snow_canvas_text_editor_input::planFontSizeWheel(
            snow_canvas_text_editor_input::FontSizeWheelRequest{
                true,
                SnowCanvasTool::Text,
                Qt::NoModifier,
                0,
                120,
            });

    require(angleWheel.matchedToolWheel, "text tool wheel should be owned by font-size policy");
    require(angleWheel.shouldStepFontSize, "non-zero text wheel delta should step font size");
    require(angleWheel.increase, "positive angle delta should increase font size");

    const snow_canvas_text_editor_input::FontSizeWheelPlan pixelWheel =
        snow_canvas_text_editor_input::planFontSizeWheel(
            snow_canvas_text_editor_input::FontSizeWheelRequest{
                true,
                SnowCanvasTool::Text,
                Qt::NoModifier,
                -1,
                120,
            });

    require(pixelWheel.shouldStepFontSize, "pixel wheel delta should step font size");
    require(!pixelWheel.increase, "negative pixel delta should decrease font size");

    const snow_canvas_text_editor_input::FontSizeWheelPlan zoomWheel =
        snow_canvas_text_editor_input::planFontSizeWheel(
            snow_canvas_text_editor_input::FontSizeWheelRequest{
                true,
                SnowCanvasTool::Text,
                Qt::ControlModifier,
                0,
                -120,
            });

    require(!zoomWheel.matchedToolWheel,
            "modified wheel should remain available for generic viewport input");

    const snow_canvas_text_editor_input::FontSizeWheelPlan zeroDelta =
        snow_canvas_text_editor_input::planFontSizeWheel(
            snow_canvas_text_editor_input::FontSizeWheelRequest{
                true,
                SnowCanvasTool::Text,
                Qt::NoModifier,
                0,
                0,
            });

    require(zeroDelta.matchedToolWheel,
            "zero-delta text wheel should still match font-size policy");
    require(!zeroDelta.shouldStepFontSize,
            "zero-delta text wheel should not request a font-size mutation");

    const snow_canvas_text_editor_input::FontSizeWheelPlan serialWheel =
        snow_canvas_text_editor_input::planFontSizeWheel(
            snow_canvas_text_editor_input::FontSizeWheelRequest{
                true,
                SnowCanvasTool::SerialNumber,
                Qt::NoModifier,
                0,
                -120,
            });
    require(serialWheel.matchedToolWheel,
            "serial-number tool wheel should be owned by font-size policy");
    require(serialWheel.shouldStepFontSize, "serial-number tool wheel should step font size");
    require(!serialWheel.increase, "negative serial-number wheel delta should decrease font size");

    requireNear(snow_canvas_text_measurement::steppedFontSize(21.0, true), 22.0,
                "text wheel increase should use a one-pixel step");
    requireNear(snow_canvas_text_measurement::steppedFontSize(21.0, false), 20.0,
                "text wheel decrease should use a one-pixel step");
    requireNear(snow_canvas_text_measurement::steppedFontSize(6.0, false), 6.0,
                "text wheel decrease should stop at the minimum font size");
    requireNear(snow_canvas_text_measurement::steppedFontSize(256.0, true), 256.0,
                "text wheel increase should stop at the maximum font size");
}

void sceneRectangleRendererBuildsFillPathForEveryCornerStyle() {
    require(renderedSceneRectangleFillPixelCount(SnowCornerRadii{}) > 0,
            "square scene rectangles should render patterned fills");
    require(renderedSceneRectangleFillPixelCount(SnowCornerRadii{6.0, 6.0, 6.0, 6.0}) > 0,
            "uniformly rounded scene rectangles should render patterned fills");
}

void textBackgroundUsesRectangleHatchTexture() {
    constexpr double fontSize = 30.0;
    const std::size_t cacheEntriesBefore =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int linePixels = renderedTextBackgroundPixelCount(SNOW_FILL_STYLE_LINE, fontSize);
    const std::size_t cacheEntriesAfterText =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int crossLinePixels =
        renderedTextBackgroundPixelCount(SNOW_FILL_STYLE_CROSS_LINE, fontSize);

    require(linePixels > 0, "line text fill should render hatch pixels");
    require(crossLinePixels > linePixels,
            "cross-line text fill should render both hatch directions");
    require(cacheEntriesAfterText <= cacheEntriesBefore + 1,
            "text fill should create at most one semantic hatch texture");

    require(renderedOverlayFillPixelCount(SNOW_FILL_STYLE_LINE, fontSize / 24.0) > 0,
            "equivalent rectangle stroke width should render hatch pixels");
    require(snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread() ==
                cacheEntriesAfterText,
            "twenty-four pixels of text size should share one rectangle stroke-width texture");
}

void serialNumberBackgroundUsesTextHatchTexture() {
    constexpr double fontSize = 30.0;
    renderedTextBackgroundPixelCount(SNOW_FILL_STYLE_LINE, fontSize);
    const std::size_t cacheEntriesAfterText =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int linePixels = renderedSerialNumberBackgroundPixelCount(SNOW_FILL_STYLE_LINE, fontSize);
    const std::size_t cacheEntriesAfterSerialNumber =
        snow_canvas_renderer::hatchTextureCacheEntryCountForCurrentThread();
    const int crossLinePixels =
        renderedSerialNumberBackgroundPixelCount(SNOW_FILL_STYLE_CROSS_LINE, fontSize);
    const int solidPixels =
        renderedSerialNumberBackgroundPixelCount(SNOW_FILL_STYLE_SOLID, fontSize);

    require(linePixels > 0, "line serial-number fill should render hatch pixels");
    require(crossLinePixels > linePixels,
            "cross-line serial-number fill should render both hatch directions");
    require(solidPixels > crossLinePixels,
            "non-solid serial-number fills should retain transparent gaps");
    require(cacheEntriesAfterSerialNumber == cacheEntriesAfterText,
            "serial-number and text backgrounds should share one hatch texture");
}

void multilineTextHoverRendererDrawsEveryLineUnderline() {
    QImage image(QSize(160, 160), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowCanvasSceneItem textItem;
    textItem.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    textItem.center_x = 0.0;
    textItem.center_y = 0.0;
    textItem.width = 100.0;
    textItem.height = 90.0;
    textItem.font_size = 18.0;
    textItem.opacity = 1.0;
    textItem.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    textItem.text_vertical_align = SNOW_TEXT_VERTICAL_ALIGN_TOP;
    snow_canvas_text::copyTextToSceneItem(textItem, QStringLiteral("a\nb\nc"));

    SnowCanvasOverlayItem hoverItem;
    hoverItem.kind = SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT;
    hoverItem.rect_kind = SNOW_OVERLAY_RECT_TEXT_HOVER_UNDERLINE;
    hoverItem.center_x = textItem.center_x;
    hoverItem.center_y = textItem.center_y;
    hoverItem.width = textItem.width;
    hoverItem.height = textItem.height;
    hoverItem.rotation = textItem.rotation;
    hoverItem.stroke = SnowColorRgba8{0x40, 0x96, 0xff, 0xff};
    hoverItem.stroke_width = 1.5;

    SceneDisplayInfo sceneInfo{};
    sceneInfo.item_count = 1;
    sceneInfo.surface_width = image.width();
    sceneInfo.surface_height = image.height();
    sceneInfo.camera_zoom = 1.0;
    OverlayDisplayInfo overlayInfo{};
    overlayInfo.item_count = 1;
    overlayInfo.surface_width = image.width();
    overlayInfo.surface_height = image.height();
    overlayInfo.camera_zoom = 1.0;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderOverlayItems(painter, overlayInfo, &hoverItem, 1,
                                             QRegion(image.rect()), &sceneInfo, &textItem, 1);
    painter.end();

    int underlineRows = 0;
    bool previousRowContainsBlue = false;
    for (int y = 0; y < image.height(); ++y) {
        const bool rowContainsBlue =
            imageRegionContainsSelectionBlue(image, QRect(0, y, image.width(), 1));
        if (rowContainsBlue && !previousRowContainsBlue) {
            ++underlineRows;
        }
        previousRowContainsBlue = rowContainsBlue;
    }
    require(underlineRows == 3,
            "multiline text hover feedback should draw one underline for every text line");
}

void compactSceneItemsOwnBorrowedPatchData() {
    QByteArray text = QByteArrayLiteral("owned text");
    std::vector<SnowArrowPoint> points{{1.0, 2.0}, {3.0, 4.0}};
    SnowSceneDisplayItem borrowed{};
    borrowed.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    borrowed.text_utf8 = text.constData();
    borrowed.text_utf8_len = static_cast<std::uint32_t>(text.size());
    borrowed.arrow_points = points.data();
    borrowed.arrow_point_count = static_cast<std::uint32_t>(points.size());

    SnowCanvasSceneItem owned(borrowed);
    text.fill('x');
    points.clear();

    require(snow_canvas_text::textFromSceneItem(owned) == QStringLiteral("owned text"),
            "owned scene item should outlive borrowed UTF-8 storage");
    require(owned.arrow_point_count == 2, "owned scene item should retain borrowed arrow points");
    requireNear(owned.arrow_points[1].x, 3.0, "owned arrow point x");
}

void adaptiveRepaintKeepsSparseRegionsSeparate() {
    const QRect clip(0, 0, 1000, 1000);
    QRegion sparse(QRect(10, 10, 20, 20));
    sparse += QRect(900, 900, 20, 20);
    const QRegion adaptive = snow_canvas_widget_repaint::adaptiveUpdateRegion(sparse, clip);
    require(adaptive.rectCount() > 1, "distant dirty regions should remain separate");
    require(!adaptive.contains(QPoint(500, 500)), "sparse repaint should not cover the gap");

    QRegion nearby(QRect(10, 10, 20, 20));
    nearby += QRect(32, 10, 20, 20);
    const QRegion coalesced = snow_canvas_widget_repaint::adaptiveUpdateRegion(nearby, clip);
    require(coalesced.rectCount() == 1, "nearby dirty regions should use their bounding rect");
}

void filterRendererClipsEffectAndPreservesContentAboveIt() {
    QImage image(QSize(100, 100), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowSceneDisplayItem below{};
    below.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    below.width = 80.0;
    below.height = 80.0;
    below.fill = SnowColorRgba8{255, 0, 0, 255};
    below.fill_style = SNOW_FILL_STYLE_SOLID;
    below.opacity = 1.0;

    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 40.0;
    filter.height = 40.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    filter.opacity = 1.0;

    SnowSceneDisplayItem above = below;
    above.width = 10.0;
    above.height = 10.0;
    above.fill = SnowColorRgba8{0, 255, 0, 255};

    const SnowCanvasSceneItem items[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(filter),
        SnowCanvasSceneItem(above),
    };
    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 3;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;

    QPainter painter(&image);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        items,
        3,
        QRegion(image.rect()),
        nullptr,
        0,
    });
    painter.end();

    require(image.pixelColor(60, 50) == QColor(0, 255, 255),
            "inversion should affect content below inside the filter clip");
    require(image.pixelColor(85, 50) == QColor(255, 0, 0),
            "filter should not affect pixels outside its bounds");
    require(image.pixelColor(50, 50) == QColor(0, 255, 0),
            "content above the filter should remain unchanged");
}

void filterRendererAppliesToBackgroundContent() {
    QImage image(QSize(100, 100), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QImage background(image.size(), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(255, 0, 0));

    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 40.0;
    filter.height = 40.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    filter.opacity = 1.0;

    const SnowCanvasSceneItem items[] = {SnowCanvasSceneItem(filter)};
    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 1;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;

    QPainter painter(&image);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        items,
        1,
        QRegion(image.rect()),
        nullptr,
        0,
        &background,
    });
    painter.end();

    require(image.pixelColor(50, 50) == QColor(0, 255, 255),
            "a filter-only scene should transform its background content");
    require(image.pixelColor(85, 50) == QColor(255, 0, 0),
            "background content outside the filter should remain unchanged");
}

void adjacentSameTypeFiltersShareTheirPreGroupScene() {
    QImage image(QSize(80, 80), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowSceneDisplayItem below{};
    below.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    below.width = 60.0;
    below.height = 60.0;
    below.fill = SnowColorRgba8{255, 0, 0, 255};
    below.fill_style = SNOW_FILL_STYLE_SOLID;
    below.opacity = 1.0;
    SnowSceneDisplayItem inversion{};
    inversion.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    inversion.width = 40.0;
    inversion.height = 40.0;
    inversion.filter = snow_filter_render_spec_resolve(3, 1.0);
    inversion.opacity = 1.0;

    const SnowCanvasSceneItem items[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(inversion),
        SnowCanvasSceneItem(inversion),
    };
    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = 3;
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;

    QPainter painter(&image);
    painter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &displayInfo, items, 3, QRegion(image.rect()), nullptr, 0});
    painter.end();

    require(image.pixelColor(40, 40) == QColor(0, 255, 255),
            "overlapping adjacent same-type filters should sample one pre-group scene");
    const snow_canvas_renderer::FilterRenderDiagnostics grouped =
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(grouped.recorderCount == 1 && grouped.filterLayerCount == 1 &&
                grouped.originalFilterCount == 2 && grouped.effectDispatchCount == 1 &&
                grouped.batchedFilterCount == 1 && grouped.maskPixelCount == 0 &&
                grouped.opaqueRectDispatchCount == 1,
            "same-effect filters should share one direct effect dispatch");
    image.fill(Qt::transparent);
    QPainter cachedPainter(&image);
    cachedPainter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &cachedPainter, &displayInfo, items, 3, QRegion(image.rect()), nullptr, 0});
    cachedPainter.end();
    const snow_canvas_renderer::FilterRenderDiagnostics cached =
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(cached.usedFilterPath && cached.workingSurfacePixelCount <= 80u * 80u,
            "an unchanged filter render should stay within the exposed surface bounds");

    SnowSceneDisplayItem offscreen = inversion;
    offscreen.center_x = 5000.0;
    const SnowCanvasSceneItem culledItems[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(offscreen),
    };
    image.fill(Qt::transparent);
    QPainter culledPainter(&image);
    culledPainter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &culledPainter,
        &displayInfo,
        culledItems,
        2,
        QRegion(image.rect()),
        nullptr,
        0,
    });
    culledPainter.end();
    const snow_canvas_renderer::FilterRenderDiagnostics culled =
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(culled.recorderCount == 0 && culled.layerCount == 0 && culled.filterPassCount == 0,
            "a wholly offscreen filter should stay on the zero-pass direct path");

    SnowSceneDisplayItem mosaic = inversion;
    mosaic.filter = snow_filter_render_spec_resolve(0, 0.5);
    const SnowCanvasSceneItem mosaicItems[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(mosaic),
    };
    image.fill(Qt::transparent);
    QPainter mosaicPainter(&image);
    mosaicPainter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &mosaicPainter,
        &displayInfo,
        mosaicItems,
        2,
        QRegion(image.rect()),
        nullptr,
        0,
    });
    mosaicPainter.end();
    require(image.pixelColor(40, 40) == QColor(255, 0, 0),
            "the CPU mosaic fallback should preserve visible uniform source content");
    require(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread().filterPassCount == 1,
            "the CPU mosaic fallback should report one completed filter pass");

    SnowSceneDisplayItem shiftedMosaic = mosaic;
    shiftedMosaic.center_x = 100.0;
    shiftedMosaic.center_y = 100.0;
    SnowSceneDisplayItem shiftedBelow = below;
    shiftedBelow.center_x = 100.0;
    shiftedBelow.center_y = 100.0;
    const SnowCanvasSceneItem shiftedMosaicItems[] = {
        SnowCanvasSceneItem(shiftedBelow),
        SnowCanvasSceneItem(shiftedMosaic),
    };
    SceneDisplayInfo shiftedDisplayInfo = displayInfo;
    shiftedDisplayInfo.camera_center_x = 100.0;
    shiftedDisplayInfo.camera_center_y = 100.0;
    image.fill(Qt::transparent);
    QPainter shiftedMosaicPainter(&image);
    shiftedMosaicPainter.setClipRect(image.rect());
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &shiftedMosaicPainter,
        &shiftedDisplayInfo,
        shiftedMosaicItems,
        2,
        QRegion(image.rect()),
        nullptr,
        0,
    });
    shiftedMosaicPainter.end();
    require(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread().filterPassCount == 1,
            "mosaic rendering should tolerate a negative tile origin");
}

void watermarkRendererIsViewportAnchoredAndSkipsLowAlpha() {
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    const std::size_t buildsBefore =
        snow_canvas_renderer::watermarkLayoutCacheBuildCountForCurrentThread();
    WatermarkDisplayInfo info{};
    info.surface_width = 320;
    info.surface_height = 180;
    info.watermark_color = SnowColorRgba8{20, 40, 60, 255};
    const QByteArray text("DRAFT");
    std::copy(text.begin(), text.end(), info.watermark_text.begin());
    info.watermark_text_len = static_cast<std::uint16_t>(text.size());
    info.watermark_font_size = 21;
    info.watermark_gap = 40;
    info.watermark_angle = 30;
    info.watermark_opacity = 0.5;
    QImage first(320, 180, QImage::Format_ARGB32_Premultiplied);
    first.fill(Qt::transparent);
    QPainter firstPainter(&first);
    snow_canvas_renderer::renderWatermark(firstPainter, info);
    firstPainter.end();
    const std::size_t buildsAfterFirst =
        snow_canvas_renderer::watermarkLayoutCacheBuildCountForCurrentThread();
    require(buildsAfterFirst == buildsBefore + 1,
            "first visible watermark render should build its text layout and tile once");
    const snow_canvas_renderer::WatermarkRenderDiagnostics firstDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(firstDiagnostics.renderCallCount == 1 && firstDiagnostics.shapeMissCount == 1 &&
                firstDiagnostics.unitMissCount == 1 && firstDiagnostics.tintBuildCount == 1 &&
                firstDiagnostics.selectedStrategy ==
                    snow_canvas_renderer::WatermarkRenderStrategy::SparseImage &&
                firstDiagnostics.submittedFragmentCount > 0 &&
                firstDiagnostics.fallbackGlyphDrawCount == 0,
            "a cold watermark render should report shaping, rasterization, tint, and composition");

    WatermarkDisplayInfo secondCanvasInfo = info;
    secondCanvasInfo.surface_width = 640;
    secondCanvasInfo.surface_height = 360;
    QImage secondCanvas(640, 360, QImage::Format_ARGB32_Premultiplied);
    secondCanvas.fill(Qt::transparent);
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QPainter secondCanvasPainter(&secondCanvas);
    snow_canvas_renderer::renderWatermark(secondCanvasPainter, secondCanvasInfo);
    secondCanvasPainter.end();
    const snow_canvas_renderer::WatermarkRenderDiagnostics reusedDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(reusedDiagnostics.shapeHitCount == 1 && reusedDiagnostics.unitHitCount == 1 &&
                reusedDiagnostics.shapeMissCount == 0 && reusedDiagnostics.unitMissCount == 0 &&
                reusedDiagnostics.tintBuildCount == 0,
            "multiple canvases should reuse the process-wide shaped and tinted unit");
    const auto hasVisiblePixel = [](const QImage& image) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).alpha() > 0) {
                    return true;
                }
            }
        }
        return false;
    };

    const QRect partialArea(90, 50, 140, 80);
    QImage partial(320, 180, QImage::Format_ARGB32_Premultiplied);
    partial.fill(Qt::transparent);
    QPainter partialPainter(&partial);
    snow_canvas_renderer::renderWatermark(partialPainter, info, partialArea);
    partialPainter.end();
    require(hasVisiblePixel(partial.copy(partialArea)),
            "a partial watermark area should contain visible watermark pixels");
    QImage outsidePartial = partial;
    {
        QPainter outsidePainter(&outsidePartial);
        outsidePainter.setCompositionMode(QPainter::CompositionMode_Source);
        outsidePainter.fillRect(partialArea, Qt::transparent);
    }
    require(!hasVisiblePixel(outsidePartial),
            "a partial watermark area must clip every watermark pixel outside it");

    info.camera_center_x = 1000;
    info.camera_center_y = -500;
    info.camera_zoom = 4;
    QImage second(320, 180, QImage::Format_ARGB32_Premultiplied);
    second.fill(Qt::transparent);
    QPainter secondPainter(&second);
    snow_canvas_renderer::renderWatermark(secondPainter, info);
    secondPainter.end();
    require(first == second, "camera pan and zoom must not move or scale the watermark");
    require(snow_canvas_renderer::watermarkLayoutCacheBuildCountForCurrentThread() ==
                buildsAfterFirst,
            "camera changes should not rebuild viewport-anchored watermark text layout");

    info.watermark_color = SnowColorRgba8{180, 30, 90, 255};
    info.watermark_angle = -48;
    info.watermark_opacity = 0.7;
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage retinted(320, 180, QImage::Format_ARGB32_Premultiplied);
    retinted.fill(Qt::transparent);
    QPainter retintedPainter(&retinted);
    snow_canvas_renderer::renderWatermark(retintedPainter, info);
    retintedPainter.end();
    require(
        snow_canvas_renderer::watermarkLayoutCacheBuildCountForCurrentThread() == buildsAfterFirst,
        "color, opacity, and angle changes should reuse watermark text layout and tile geometry");
    const snow_canvas_renderer::WatermarkRenderDiagnostics retintDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(retintDiagnostics.shapeHitCount == 1 && retintDiagnostics.unitHitCount == 1 &&
                retintDiagnostics.unitMissCount == 0 && retintDiagnostics.tintBuildCount == 1 &&
                retintDiagnostics.selectedStrategy !=
                    snow_canvas_renderer::WatermarkRenderStrategy::None,
            "color and opacity changes should rebuild only the tinted watermark unit");

    info.watermark_angle = 12;
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage rerotated(320, 180, QImage::Format_ARGB32_Premultiplied);
    rerotated.fill(Qt::transparent);
    QPainter rerotatedPainter(&rerotated);
    snow_canvas_renderer::renderWatermark(rerotatedPainter, info);
    rerotatedPainter.end();
    const snow_canvas_renderer::WatermarkRenderDiagnostics angleDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(angleDiagnostics.shapeHitCount == 1 && angleDiagnostics.unitHitCount == 1 &&
                angleDiagnostics.unitMissCount == 0 && angleDiagnostics.tintBuildCount == 0 &&
                angleDiagnostics.selectedStrategy !=
                    snow_canvas_renderer::WatermarkRenderStrategy::None,
            "angle-only changes should reuse every watermark cache layer");

    info.watermark_gap = 120;
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage regapped(320, 180, QImage::Format_ARGB32_Premultiplied);
    regapped.fill(Qt::transparent);
    QPainter regappedPainter(&regapped);
    snow_canvas_renderer::renderWatermark(regappedPainter, info);
    regappedPainter.end();
    const auto gapDiagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(gapDiagnostics.unitHitCount == 1 && gapDiagnostics.unitMissCount == 0 &&
                gapDiagnostics.tintBuildCount == 0,
            "gap-only changes should reuse shaping, alpha, and tint unit data");

    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage dprImage(QSize(480, 270), QImage::Format_ARGB32_Premultiplied);
    dprImage.setDevicePixelRatio(1.5);
    dprImage.fill(Qt::transparent);
    QPainter dprPainter(&dprImage);
    snow_canvas_renderer::renderWatermark(dprPainter, info);
    dprPainter.end();
    const auto dprDiagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(dprDiagnostics.shapeHitCount == 1 && dprDiagnostics.unitMissCount == 1,
            "fractional DPR should reuse shaping while building a distinct physical unit");

    const std::size_t fallbacksBefore =
        snow_canvas_renderer::watermarkDirectFallbackCountForCurrentThread();
    const QByteArray oversizedText(255, 'W');
    std::copy(oversizedText.begin(), oversizedText.end(), info.watermark_text.begin());
    info.watermark_text_len = static_cast<std::uint16_t>(oversizedText.size());
    info.watermark_font_size = 42;
    info.watermark_gap = 10;
    info.watermark_angle = 0;
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage oversized(320, 180, QImage::Format_ARGB32_Premultiplied);
    oversized.fill(Qt::transparent);
    QPainter oversizedPainter(&oversized);
    snow_canvas_renderer::renderWatermark(oversizedPainter, info);
    oversizedPainter.end();
    const snow_canvas_renderer::WatermarkRenderDiagnostics segmentedDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(snow_canvas_renderer::watermarkDirectFallbackCountForCurrentThread() ==
                    fallbacksBefore &&
                segmentedDiagnostics.selectedStrategy ==
                    snow_canvas_renderer::WatermarkRenderStrategy::SegmentedSparse &&
                segmentedDiagnostics.segmentedChunkCount > 0,
            "a wide unit should render as bounded overlapping chunks without glyph fallback");

    info.watermark_text.fill(0);
    info.watermark_text[0] = 'W';
    info.watermark_text_len = 1;
    info.watermark_font_size = 2000;
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage fallback(320, 180, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(Qt::transparent);
    QPainter fallbackPainter(&fallback);
    snow_canvas_renderer::renderWatermark(fallbackPainter, info);
    fallbackPainter.end();
    const auto fallbackDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(snow_canvas_renderer::watermarkDirectFallbackCountForCurrentThread() ==
                    fallbacksBefore + 1 &&
                fallbackDiagnostics.selectedStrategy ==
                    snow_canvas_renderer::WatermarkRenderStrategy::GlyphFallback &&
                fallbackDiagnostics.fallbackGlyphDrawCount > 0,
            "a glyph exceeding the chunk dimension should use clipped glyph-run fallback");
    info.watermark_opacity = 0.003;
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    QImage hidden(320, 180, QImage::Format_ARGB32_Premultiplied);
    hidden.fill(Qt::transparent);
    QPainter hiddenPainter(&hidden);
    snow_canvas_renderer::renderWatermark(hiddenPainter, info);
    hiddenPainter.end();
    require(!hasVisiblePixel(hidden),
            "effective alpha below the visibility threshold should render nothing");
    const snow_canvas_renderer::WatermarkRenderDiagnostics hiddenDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(hiddenDiagnostics.renderCallCount == 1 && hiddenDiagnostics.earlyExitCount == 1 &&
                hiddenDiagnostics.selectedStrategy ==
                    snow_canvas_renderer::WatermarkRenderStrategy::None,
            "hidden watermarks should report a cheap early exit");

    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
    snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
    for (int index = 0; index < 24; ++index) {
        WatermarkDisplayInfo cacheEntryInfo = info;
        const QByteArray cacheText = QStringLiteral("CACHE %1").arg(index).toUtf8();
        std::copy(cacheText.begin(), cacheText.end(), cacheEntryInfo.watermark_text.begin());
        cacheEntryInfo.watermark_text_len = static_cast<std::uint16_t>(cacheText.size());
        cacheEntryInfo.watermark_font_size = 18;
        cacheEntryInfo.watermark_opacity = 0.5;
        QImage cacheImage(96, 64, QImage::Format_ARGB32_Premultiplied);
        cacheImage.fill(Qt::transparent);
        QPainter cachePainter(&cacheImage);
        snow_canvas_renderer::renderWatermark(cachePainter, cacheEntryInfo);
        cachePainter.end();
    }
    const snow_canvas_renderer::WatermarkRenderDiagnostics evictionDiagnostics =
        snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
    require(evictionDiagnostics.cacheEvictionCount > 0 &&
                snow_canvas_renderer::watermarkPatternCacheEntryCountForCurrentThread() <= 8 &&
                snow_canvas_renderer::watermarkPatternCacheBytesForCurrentThread() <=
                    16u * 1024u * 1024u,
            "watermark pattern cache should evict within its entry and byte bounds");
}

void watermarkPreviewCoalescesAndCommitCancelsQueuedPreview() {
    SnowCanvasWidget canvas;
    int appliedCount = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::watermarkPreviewApplied,
                     [&appliedCount]() { ++appliedCount; });

    SnowCanvasWatermarkConfig first;
    first.text = QStringLiteral("FIRST");
    SnowCanvasWatermarkConfig latest = first;
    latest.text = QStringLiteral("LATEST");
    latest.angle = 72.0;
    canvas.previewCanvasWatermarkConfig(first);
    require(appliedCount == 1, "the first watermark preview should apply synchronously");
    canvas.previewCanvasWatermarkConfig(latest);
    canvas.previewCanvasWatermarkConfig(latest);
    require(appliedCount == 1,
            "later preview writes should coalesce until the refresh-paced callback");
    QElapsedTimer previewWait;
    previewWait.start();
    while (appliedCount < 2 && previewWait.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(appliedCount == 2,
            "the refresh-paced callback should deliver the latest pending preview once");

    SnowCanvasWatermarkConfig stale = latest;
    stale.text = QStringLiteral("STALE");
    SnowCanvasWatermarkConfig committed = latest;
    committed.text = QStringLiteral(" COMMITTED ");
    canvas.previewCanvasWatermarkConfig(stale);
    require(canvas.setCanvasWatermarkConfig(committed),
            "watermark commit should succeed while a preview is queued");
    QCoreApplication::processEvents();
    require(appliedCount == 2,
            "a persistent watermark commit should cancel its queued transient preview");
    require(canvas.canvasWatermarkConfig().text == QStringLiteral("COMMITTED"),
            "the committed normalized watermark should remain authoritative");

    {
        auto destroyedWithPendingPreview = std::make_unique<SnowCanvasWidget>();
        destroyedWithPendingPreview->previewCanvasWatermarkConfig(first);
        destroyedWithPendingPreview->previewCanvasWatermarkConfig(latest);
        destroyedWithPendingPreview.reset();
        QElapsedTimer destructionWait;
        destructionWait.start();
        while (destructionWait.elapsed() < 25) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
    }
}

void watermarkCacheSupportsConcurrentImagesAndGuiPixmapLifecycle() {
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
    WatermarkDisplayInfo info{};
    info.surface_width = 320;
    info.surface_height = 180;
    info.watermark_color = SnowColorRgba8{20, 80, 140, 255};
    const QByteArray text("CONCURRENT");
    std::copy(text.begin(), text.end(), info.watermark_text.begin());
    info.watermark_text_len = static_cast<std::uint16_t>(text.size());
    info.watermark_font_size = 20;
    info.watermark_gap = 32;
    info.watermark_angle = 24;
    info.watermark_opacity = 0.5;

    std::atomic<bool> rendered{true};
    std::atomic<int> completedWorkers{0};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int workerIndex = 0; workerIndex < 4; ++workerIndex) {
        workers.emplace_back([info, workerIndex, &rendered, &completedWorkers]() mutable {
            info.watermark_angle += workerIndex;
            for (int iteration = 0; iteration < 12; ++iteration) {
                QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                snow_canvas_renderer::renderWatermark(painter, info);
                painter.end();
                bool visible = false;
                for (int y = 0; y < image.height() && !visible; ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (qAlpha(image.pixel(x, y)) != 0) {
                            visible = true;
                            break;
                        }
                    }
                }
                if (!visible) {
                    rendered.store(false, std::memory_order_relaxed);
                }
            }
            completedWorkers.fetch_add(1, std::memory_order_release);
        });
    }
    while (completedWorkers.load(std::memory_order_acquire) < 4) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(rendered.load(std::memory_order_relaxed) &&
                snow_canvas_renderer::watermarkPatternCacheEntryCountForCurrentThread() <= 8 &&
                snow_canvas_renderer::watermarkPatternCacheBytesForCurrentThread() <=
                    16u * 1024u * 1024u,
            "concurrent image rendering should share a bounded, complete process cache");

    QImage widgetTarget(320, 180, QImage::Format_ARGB32_Premultiplied);
    widgetTarget.fill(Qt::transparent);
    QPainter widgetPainter(&widgetTarget);
    snow_canvas_renderer::WatermarkPatternRenderer::render(
        widgetPainter, snow_canvas_renderer::WatermarkRenderRequest{
                           info,
                           QRectF(0, 0, 320, 180),
                           QRectF(0, 0, 320, 180),
                           QRegion(QRect(0, 0, 320, 180)),
                           widgetPainter.deviceTransform(),
                           snow_canvas_renderer::WatermarkRenderPurpose::Widget,
                       });
    widgetPainter.end();
    std::thread eviction(
        []() { snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread(); });
    eviction.join();
    QCoreApplication::processEvents();
    require(snow_canvas_renderer::watermarkPatternCacheEntryCountForCurrentThread() == 0,
            "worker eviction should release GUI mirrors through the GUI event queue");
}

void watermarkIsIncludedInRuntimeExport() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    SnowCanvasWatermarkConfig config;
    config.text = QStringLiteral("EXPORT");
    config.color = Qt::black;
    config.fontSize = 18.0;
    config.angle = 0.0;
    config.gap = 10.0;
    config.opacity = 1.0;
    require(canvas.setCanvasWatermarkConfig(config),
            "the export test should configure a visible watermark");

    const QRectF selection(-60.0, -40.0, 120.0, 80.0);
    QImage background(120, 80, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    const QImage output = runtime.renderToImage(selection, background.size(),
                                                {CanvasExportSource{background, selection}});
    require(!output.isNull(), "a valid runtime export should produce an image");
    require(output.format() == QImage::Format_ARGB32_Premultiplied,
            "runtime export should return its premultiplied working surface directly");

    bool hasWatermarkPixel = false;
    for (int y = 0; y < output.height() && !hasWatermarkPixel; ++y) {
        for (int x = 0; x < output.width(); ++x) {
            if (output.pixelColor(x, y).lightness() < 240) {
                hasWatermarkPixel = true;
                break;
            }
        }
    }
    require(hasWatermarkPixel,
            "runtime export should composite the configured watermark over its sources");

    QByteArray encoded;
    QBuffer buffer(&encoded);
    require(buffer.open(QIODevice::WriteOnly) && output.save(&buffer, "PNG"),
            "callers should be able to save the premultiplied export without conversion");
    const QImage decoded = QImage::fromData(encoded, "PNG");
    require(!decoded.isNull() && decoded.size() == output.size(),
            "a saved premultiplied export should decode with its dimensions intact");

    QImage pinned = output;
    const QColor originalCorner = output.pixelColor(0, 0);
    {
        QPainter stylePainter(&pinned);
        stylePainter.fillRect(QRect(0, 0, 4, 4), Qt::red);
    }
    require(output.pixelColor(0, 0) == originalCorner && pinned.pixelColor(0, 0) != originalCorner,
            "styling a pinned export copy should detach without mutating the returned image");
    QImage repainted(output.size(), QImage::Format_ARGB32_Premultiplied);
    repainted.fill(Qt::transparent);
    {
        QPainter repaintPainter(&repainted);
        repaintPainter.drawImage(QPoint(0, 0), output);
    }
    require(!repainted.isNull() && repainted.pixelColor(0, 0).alpha() > 0,
            "the returned premultiplied export should repaint through a normal Qt painter");
}

void freeDrawMoveBurstsAreFrameBounded() {
    SnowCanvasWidget canvas;
    canvas.resize(320, 240);
    require(canvas.setCanvasTool(SnowCanvasTool::FreeDraw),
            "free-draw batching test should activate the tool");
    quint32 observedInputCount = 0;
    quint32 observedDispatchCount = 0;
    int batchCount = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::freeDrawMoveBatchProcessed,
                     [&observedInputCount, &observedDispatchCount,
                      &batchCount](quint32 inputCount, quint32 dispatchedCount) {
                         observedInputCount = inputCount;
                         observedDispatchCount = dispatchedCount;
                         ++batchCount;
                     });

    sendCanvasMouseEvent(canvas, QEvent::MouseButtonPress, QPointF(20.0, 120.0), Qt::LeftButton,
                         Qt::LeftButton);
    for (int index = 0; index < 240; ++index) {
        sendCanvasMouseEvent(canvas, QEvent::MouseMove, QPointF(20.0 + index, 120.0 + (index % 7)),
                             Qt::NoButton, Qt::LeftButton,
                             (index >= 80 && index < 160) ? Qt::ShiftModifier : Qt::NoModifier);
    }
    require(batchCount == 0,
            "free-draw move bursts should remain queued until the event-loop frame boundary");
    QElapsedTimer batchWait;
    batchWait.start();
    while (batchCount == 0 && batchWait.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(batchCount == 1 && observedInputCount == 240 && observedDispatchCount == 96,
            "one free-draw frame should uniformly reduce a burst to the 96-sample cap");

    sendCanvasMouseEvent(canvas, QEvent::MouseButtonRelease, QPointF(280.0, 120.0), Qt::LeftButton,
                         Qt::NoButton);
}

void eraserMoveBurstsUseTheLatestSamplePerFrame() {
    SnowCanvasWidget canvas;
    canvas.resize(320, 240);
    require(canvas.setCanvasTool(SnowCanvasTool::Eraser),
            "eraser frame test should activate the tool");
    int processedFrames = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::eraserMoveFrameProcessed,
                     [&processedFrames]() { ++processedFrames; });

    sendCanvasMouseEvent(canvas, QEvent::MouseButtonPress, QPointF(20.0, 120.0), Qt::LeftButton,
                         Qt::LeftButton);
    for (int index = 0; index < 120; ++index) {
        sendCanvasMouseEvent(canvas, QEvent::MouseMove, QPointF(20.0 + index, 120.0), Qt::NoButton,
                             Qt::LeftButton);
    }
    require(processedFrames == 0,
            "eraser move bursts should wait for the frame-aligned dispatcher");
    QCoreApplication::processEvents();
    require(processedFrames == 1,
            "one event-loop frame should process only the latest queued eraser move");
    sendCanvasMouseEvent(canvas, QEvent::MouseButtonRelease, QPointF(160.0, 120.0), Qt::LeftButton,
                         Qt::NoButton);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "the watermark renderer test requires a system TrueType font");
#endif

    replacementNormalizesLineBreaksAndSupportsUndoRedo();
    cursorPositionReportsOnlyRealStateChanges();
    inputMethodPreeditDoesNotCommitUntilCommitString();
    inputMethodCommitReplacementUsesCursorRelativeRange();
    keyCommandsInsertTextAndReportEditorCommands();
    keyCommandsDelegateCursorMovement();
    keyCommandsReportNoChangeForNoopEdits();
    finishedEditCommitPolicyRequiresViewportAndContent();
    textLayoutMapsDocumentRectsToLocalItemRects();
    resizedAlignedTextKeepsCaretClickAndSelectionGeometryConsistent();
    textFontSizeFallbackIsSharedAcrossCreationAndMeasurement();
    serialNumberBoundTextMeasurementUsesCreatedTextStyle();
    bahnschriftCondensedMeasurementUsesDocumentHeight();
    bahnschriftCondensedSerialNumberUsesResolvedGlyphBounds();
    textElementInfoDecodingBuildsOwnedPreview();
    textSceneItemCopyPreservesCompleteUtf8();
    textStyleConversionsTruncateFontFamilyAtUtf8Boundaries();
    textStyleStateEqualityBoundsFontFamilyLengths();
    demoSerialNumberControlsRespectSelectedMixedDecreaseState();
    demoSerialNumberControlsClampDefaultDecrease();
    textMeasurementBuildsAutoResizeLayoutOverridesFromSnapshots();
    textEditTargetResolvesCreateHitAndSelectedText();
    textEditGeometryAnchorsRespectHorizontalAlignment();
    textEditorViewMapsCanvasHitTestingAndEditingRegion();
    textEditorCaretBlinksResetsAndHonorsSystemFlashTime();
    selectionHitTestingUsesMinimumHandleHitSize();
    selectionHitTestingTreatsTextFramePaddingAsMoveRing();
    textEditorActivationPreservesSelectToolSelectionBox();
    newTextDraftClearsPreviouslySelectedText();
    selectToolDragRendersSelectionMarquee();
    textToolInitialSelectionFrameRendersAndResizesThroughWidgetEvents();
    textEditorDoesNotSynthesizeSelectionControlsWithoutEngineOverlay();
    overlayRectangleRendererHonorsFillStyle();
    sceneRectangleRendererBuildsFillPathForEveryCornerStyle();
    closedLineRendererHonorsFillStyleAndExactClosure();
    longOpenPathsRenderAllCommands();
    textBackgroundUsesRectangleHatchTexture();
    serialNumberBackgroundUsesTextHatchTexture();
    textHoverUnderlineRendererDrawsOnlyTheUnderline();
    multilineTextHoverRendererDrawsEveryLineUnderline();
    hatchTextureCacheReusesSaturatedStrokeWidths();
    textEditorConnectorBuildsSerialBoundConnector();
    textEditorStylePopupInteractionPreservesDraftUntilItCloses();
    cancelingAnActiveTextDraftDoesNotCommitIt();
    inputMethodEnablementTracksInlineTextEditing();
    widgetPointerFlowPlansSuppressedTextCreate();
    widgetPointerFlowDispatchesShiftClickInTextToolToEngine();
    widgetPointerFlowDispatchesTextToolSelectionPressWithoutCommit();
    widgetPointerFlowDispatchesSelectToolSelectionPressWithoutCommit();
    widgetPointerFlowDispatchesAnyToolSelectionPressWithoutCommit();
    textEditorWheelPlansFontSizeStepping();
    compactSceneItemsOwnBorrowedPatchData();
    adaptiveRepaintKeepsSparseRegionsSeparate();
    filterRendererClipsEffectAndPreservesContentAboveIt();
    filterRendererAppliesToBackgroundContent();
    watermarkRendererIsViewportAnchoredAndSkipsLowAlpha();
    watermarkCacheSupportsConcurrentImagesAndGuiPixmapLifecycle();
    adjacentSameTypeFiltersShareTheirPreGroupScene();
    watermarkPreviewCoalescesAndCommitCancelsQueuedPreview();
    watermarkIsIncludedInRuntimeExport();
    freeDrawMoveBurstsAreFrameBounded();
    eraserMoveBurstsUseTheLatestSamplePerFrame();
    return 0;
}
