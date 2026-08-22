#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtexteditingsession.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/screenshottabledocument.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QTextCursor>
#include <QTextBlockFormat>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ScreenshotOcrLine line(const QString& text, qreal left, qreal top) {
    ScreenshotOcrLine output;
    output.text = text;
    output.quad = QPolygonF({
        QPointF(left, top),
        QPointF(left + static_cast<qreal>(text.size()) * 10.0, top),
        QPointF(left + static_cast<qreal>(text.size()) * 10.0, top + 10.0),
        QPointF(left, top + 10.0),
    });
    return output;
}

void textSelectionFollowsMouseCharacterRange() {
    ScreenshotOcrPresentation presentation;
    presentation.lines.push_back(line(QStringLiteral("AB"), 0.0, 0.0));
    presentation.lines.push_back(line(QStringLiteral("CD"), 0.0, 20.0));

    presentation.beginTextSelection(QPointF(4.0, 5.0));
    require(!presentation.hasTextSelection(), "mouse press should establish only a caret anchor");
    presentation.finishTextSelection();
    require(!presentation.hasTextSelection(),
            "a click without a drag should not select the whole OCR line");

    presentation.beginTextSelection(ScreenshotOcrTextPosition{0, 1});
    presentation.updateTextSelection(ScreenshotOcrTextPosition{0, 2});
    presentation.finishTextSelection();
    require(presentation.selectedText() == QStringLiteral("B"),
            "a drag within one line should select only the covered characters");
    const ScreenshotOcrTextRange firstLineRange = presentation.textSelectionForLine(0);
    require(firstLineRange.start == 1 && firstLineRange.length == 1,
            "the rendered selection range should retain exact character offsets");

    presentation.beginTextSelection(ScreenshotOcrTextPosition{0, 1});
    presentation.updateTextSelection(ScreenshotOcrTextPosition{1, 1});
    presentation.finishTextSelection();
    require(presentation.selectedText() == QStringLiteral("B\nC"),
            "a forward cross-line drag should preserve partial endpoint lines");

    presentation.beginTextSelection(ScreenshotOcrTextPosition{1, 1});
    presentation.updateTextSelection(ScreenshotOcrTextPosition{0, 1});
    presentation.finishTextSelection();
    require(presentation.selectedText() == QStringLiteral("B\nC"),
            "a reverse drag should preserve the same character range");
    require(presentation.lineSelected(0) && presentation.lineSelected(1),
            "partial endpoint lines should both render selection highlights");

    presentation.selectAll();
    require(presentation.selectedText() == QStringLiteral("AB\nCD"),
            "select all should preserve visual line breaks");
    presentation.clearTextSelection();
    require(!presentation.hasTextSelection(), "clearing should remove the text selection");
}

void lineSelectionPreservesSupplementaryCharacters() {
    ScreenshotOcrPresentation presentation;
    ScreenshotOcrLine output;
    output.text = QStringLiteral("A") + QString::fromUcs4(U"\U0001F642") + QStringLiteral("B");
    output.quad = QPolygonF({
        QPointF(0.0, 0.0),
        QPointF(30.0, 0.0),
        QPointF(30.0, 10.0),
        QPointF(0.0, 10.0),
    });
    presentation.lines.push_back(output);

    presentation.beginTextSelection(QPointF(9.0, 5.0));
    presentation.updateTextSelection(QPointF(21.0, 5.0));
    presentation.finishTextSelection();
    require(presentation.selectedText() == QString::fromUcs4(U"\U0001F642"),
            "mouse hit testing should select a supplementary character intact");
}

void verticalLineHitTestingFollowsTopToBottomGraphemes() {
    ScreenshotOcrPresentation presentation;
    ScreenshotOcrLine output;
    output.text = QStringLiteral("A") + QString::fromUcs4(U"\U0001F642") + QStringLiteral("B");
    output.direction = ScreenshotOcrTextDirection::Vertical;
    output.quad = QPolygonF({
        QPointF(10.0, 10.0),
        QPointF(30.0, 12.0),
        QPointF(26.0, 92.0),
        QPointF(6.0, 90.0),
    });
    presentation.lines.push_back(output);

    const ScreenshotOcrTextPosition start = presentation.textPositionAt(QPointF(19.8, 12.0));
    const ScreenshotOcrTextPosition afterFirst = presentation.textPositionAt(QPointF(18.5, 37.0));
    const ScreenshotOcrTextPosition afterEmoji = presentation.textPositionAt(QPointF(17.0, 65.0));
    const ScreenshotOcrTextPosition end = presentation.textPositionAt(QPointF(16.2, 90.0));
    require(start.lineIndex == 0 && start.characterIndex == 0,
            "vertical hit testing should start at the top of the OCR quad");
    require(afterFirst.characterIndex == 1,
            "vertical hit testing should advance along the quad's top-to-bottom axis");
    require(afterEmoji.characterIndex == 3,
            "vertical hit testing should preserve supplementary grapheme boundaries");
    require(end.characterIndex == output.text.size(),
            "vertical hit testing should end at the bottom of the OCR quad");

    presentation.beginTextSelection(afterFirst);
    presentation.updateTextSelection(afterEmoji);
    presentation.finishTextSelection();
    require(presentation.selectedText() == QString::fromUcs4(U"\U0001F642"),
            "vertical selection should copy the original grapheme without layout substitutions");
}

void preparedGeometryAndSelectionRevisionsAvoidRepeatedWork() {
    ScreenshotOcrPresentation presentation;
    presentation.lines.push_back(line(QStringLiteral("AB"), -80.0, -20.0));
    presentation.lines.push_back(line(QStringLiteral("CD"), 20.0, 20.0));
    presentation.prepareForRendering();

    require(presentation.lineAt(QPointF(-75.0, -15.0)) == 0 &&
                presentation.lineAt(QPointF(25.0, 25.0)) == 1 &&
                presentation.lineAt(QPointF(0.0, 0.0)) == -1,
            "prepared OCR geometry should preserve exact point-to-line hit testing");
    const ScreenshotOcrTextPosition secondLineMiddle =
        presentation.textPositionAt(QPointF(30.0, 25.0));
    require(secondLineMiddle.lineIndex == 1 && secondLineMiddle.characterIndex == 1,
            "prepared OCR geometry should preserve character hit testing");

    presentation.beginTextSelection(ScreenshotOcrTextPosition{1, 0});
    const quint64 anchorRevision = presentation.selectionRevision();
    require(presentation.textSelectionActive(),
            "beginning an OCR selection should expose its active drag state");
    presentation.updateTextSelection(ScreenshotOcrTextPosition{1, 0});
    require(presentation.selectionRevision() == anchorRevision,
            "repeating an OCR character position should not invalidate selection rendering");
    presentation.updateTextSelection(secondLineMiddle);
    require(presentation.selectionRevision() == anchorRevision + 1,
            "changing the OCR character range should invalidate selection rendering once");
    presentation.updateTextSelection(secondLineMiddle);
    require(presentation.selectionRevision() == anchorRevision + 1,
            "an unchanged OCR selection range should remain a rendering no-op");
    presentation.finishTextSelection();
    require(!presentation.textSelectionActive(),
            "finishing an OCR selection should end its active drag state");
}

CapturedDisplayModel display(const QRect& canvasRect, const QColor& color, bool active = true) {
    CapturedDisplayModel output;
    output.canvasRect = canvasRect;
    output.physicalRect = canvasRect;
    output.logicalRect = canvasRect;
    output.image = QImage(canvasRect.size(), QImage::Format_RGBA8888);
    output.image.fill(color);
    output.active = active;
    return output;
}

void sourceComposerUsesOnlyCapturedDisplayPixels() {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QRect(0, 0, 4, 2), Qt::red));
    displays.appendDisplay(display(QRect(4, 0, 4, 2), Qt::blue));
    displays.appendDisplay(display(QRect(0, 2, 4, 2), Qt::green, false));

    const QImage composed = composeScreenshotSourceSelection(displays, QRect(2, 0, 4, 2));
    require(composed.size() == QSize(4, 2), "composer should crop to the selection");
    require(composed.pixelColor(0, 0) == Qt::red, "left crop should come from display one");
    require(composed.pixelColor(1, 1) == Qt::red, "display one pixels should be retained");
    require(composed.pixelColor(2, 0) == Qt::blue, "right crop should come from display two");
    require(composed.pixelColor(3, 1) == Qt::blue, "display two pixels should be retained");

    const QImage inactive = composeScreenshotSourceSelection(displays, QRect(0, 2, 4, 2));
    require(inactive.pixelColor(0, 0).alpha() == 0,
            "inactive displays must not contribute screenshot pixels");
}

void textTransformsUseOriginalPunctuationAndLineRules() {
    ScreenshotOcrPresentation presentation;
    presentation.lines.push_back(line(QStringLiteral("A,B!"), 0.0, 0.0));
    presentation.lines.push_back(line(QStringLiteral("C?"), 0.0, 20.0));
    const QString original = snow_shot::presentation::originalOcrText(presentation);
    require(original == QStringLiteral("A,B!\nC?"), "original OCR text should join lines");
    require(snow_shot::presentation::removeOcrLineBreaks(original) == QStringLiteral("A,B!C?"),
            "line-break removal should remove only line feeds");
    const QString full = snow_shot::presentation::convertOcrPunctuation(original, true);
    const QString expectedFull = QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("B") +
                                 QChar(0xFF01) + QChar('\n') + QStringLiteral("C") + QChar(0xFF1F);
    require(full == expectedFull, "full-width conversion should affect punctuation only");
    require(snow_shot::presentation::convertOcrPunctuation(full, false) == original,
            "half-width conversion should reverse the paired punctuation map");
}

void textEditingHistoryPreservesNativeEditsAndAtomicTransforms() {
    ScreenshotOcrTextEditingSession session(QStringLiteral("recognized text"));
    QTextDocument* const document = session.document();
    require(document != nullptr && session.text() == QStringLiteral("recognized text") &&
                !session.canUndo() && !session.canRedo(),
            "a new OCR editing session should start at a clean recognized baseline");

    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" edited"));
    require(session.document() == document &&
                session.text() == QStringLiteral("recognized text edited") && session.canUndo(),
            "native editor changes should update the persistent session document and history");

    session.undo();
    require(session.text() == QStringLiteral("recognized text") && session.canRedo(),
            "undo should reverse native editor changes and expose redo");
    session.redo();
    require(session.text() == QStringLiteral("recognized text edited"),
            "redo should restore native editor changes");

    require(session.replaceText(QStringLiteral("transformed text")) &&
                session.text() == QStringLiteral("transformed text"),
            "a toolbar transformation should replace the complete draft");
    session.undo();
    require(session.text() == QStringLiteral("recognized text edited"),
            "a complete-text transformation should be reversible in one undo step");
    session.redo();
    require(session.text() == QStringLiteral("transformed text"),
            "redo should restore an atomic complete-text transformation");
}

void textEditingHistoryInvalidatesRedoBranchesAndSurvivesReentry() {
    ScreenshotOcrTextEditingSession session(QStringLiteral("original"));
    QTextDocument* const firstDocument = session.document();
    require(session.replaceText(QStringLiteral("first")),
            "the first programmatic edit should be recorded");
    require(session.replaceText(QStringLiteral("second")),
            "the second programmatic edit should be recorded separately");
    session.undo();
    require(session.text() == QStringLiteral("first") && session.canRedo(),
            "undo should expose the replaced edit as a redo branch");

    require(session.replaceText(QStringLiteral("branched")) && !session.canRedo(),
            "a new edit after undo should discard the stale redo branch");
    require(session.document() == firstDocument,
            "showing the editor again should reuse the document that owns the history");
    require(!session.replaceText(QStringLiteral("branched")),
            "feeding the document's current text back into the session should not add history");

    require(session.reset() && session.text() == QStringLiteral("original"),
            "reset should restore the recognition baseline");
    session.undo();
    require(session.text() == QStringLiteral("branched"),
            "reset should be a single reversible history operation");
    session.undo();
    require(session.text() == QStringLiteral("first"),
            "history from before editor re-entry should remain available");
}

void textEditingHistoryIgnoresEditorLayoutFormatting() {
    ScreenshotOcrTextEditingSession session(QStringLiteral("recognized"));
    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" edit"));
    require(session.text() == QStringLiteral("recognized edit") && session.canUndo() &&
                !session.canRedo(),
            "the first visible editor modification should create one text-history entry");

    QTextBlockFormat layoutFormat;
    layoutFormat.setLineHeight(4.0, QTextBlockFormat::LineDistanceHeight);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(layoutFormat);
    require(session.canUndo() && !session.canRedo(),
            "editor-only block formatting must not change text-history availability");

    session.undo();
    require(session.text() == QStringLiteral("recognized") && !session.canUndo() &&
                session.canRedo(),
            "one undo should revert the first text edit and disable Undo");
    session.redo();
    require(session.text() == QStringLiteral("recognized edit") && session.canUndo() &&
                !session.canRedo(),
            "redo should restore the text edit and reverse command availability");
}

void textEditingTransformsAccumulateAndManualEditsClearTheirState() {
    ScreenshotOcrTextEditingSession session(QStringLiteral("A,\nB!"));

    require(session.setFormatting(QStringLiteral("remove")) &&
                session.text() == QStringLiteral("A,B!") &&
                session.formatting() == QStringLiteral("remove") &&
                session.punctuation().isEmpty(),
            "line-break formatting should remain selected after it is applied");

    const QString fullWidth = QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("B") +
                              QChar(0xFF01);
    require(session.setPunctuation(QStringLiteral("full")) && session.text() == fullWidth &&
                session.formatting() == QStringLiteral("remove") &&
                session.punctuation() == QStringLiteral("full"),
            "formatting and punctuation should accumulate independently");

    const QString fullWidthWithLineBreak =
        QStringLiteral("A") + QChar(0xFF0C) + QStringLiteral("\nB") + QChar(0xFF01);
    require(session.setFormatting(QStringLiteral("keep")) &&
                session.text() == fullWidthWithLineBreak &&
                session.formatting() == QStringLiteral("keep") &&
                session.punctuation() == QStringLiteral("full"),
            "changing line-break formatting should preserve the punctuation effect");

    require(session.setPunctuation(QString{}) && session.text() == QStringLiteral("A,\nB!") &&
                session.formatting() == QStringLiteral("keep") &&
                session.punctuation().isEmpty(),
            "clearing punctuation should preserve the active formatting selection");
    static_cast<void>(session.setPunctuation(QStringLiteral("full")));

    ScreenshotOcrTextEditingSession reverseOrder(QStringLiteral("A,\nB!"));
    static_cast<void>(reverseOrder.setPunctuation(QStringLiteral("full")));
    require(reverseOrder.setFormatting(QStringLiteral("remove")) &&
                reverseOrder.text() == fullWidth &&
                reverseOrder.formatting() == QStringLiteral("remove") &&
                reverseOrder.punctuation() == QStringLiteral("full"),
            "punctuation and formatting should also accumulate in reverse selection order");
    require(reverseOrder.setFormatting(QString{}) &&
                reverseOrder.text() == fullWidthWithLineBreak &&
                reverseOrder.formatting().isEmpty() &&
                reverseOrder.punctuation() == QStringLiteral("full"),
            "clearing formatting should preserve the active punctuation selection");

    QTextCursor cursor(session.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("?"));
    require(session.text() == fullWidthWithLineBreak + QStringLiteral("?") &&
                session.formatting().isEmpty() && session.punctuation().isEmpty(),
            "a native editor change should clear displayed transform selections");

    static_cast<void>(session.setFormatting(QStringLiteral("remove")));
    static_cast<void>(session.setPunctuation(QStringLiteral("half")));
    require(!session.formatting().isEmpty() && !session.punctuation().isEmpty(),
            "transform state should be populated before reset");
    session.reset();
    require(session.text() == QStringLiteral("A,\nB!") && session.formatting().isEmpty() &&
                session.punctuation().isEmpty(),
            "reset should restore the original text and clear transform selections");
}

void translationHistoryEstablishesSuccessfulAndPartialBaselines() {
    ScreenshotOcrTextEditingSession session(QString{});
    session.replaceTextWithoutHistory(QStringLiteral("streamed translation"));
    require(session.text() == QStringLiteral("streamed translation") && !session.canUndo() &&
                !session.canRedo(),
            "streaming updates should not create one undo entry per chunk");

    session.establishBaseline(QStringLiteral("streamed translation"));
    require(session.originalText() == QStringLiteral("streamed translation") &&
                !session.canUndo() && !session.canRedo(),
            "a completed translation should become the independent reset baseline");
    require(session.replaceText(QStringLiteral("edited translation")) && session.canUndo(),
            "a completed translation should become editable with its own history");
    require(session.reset() && session.text() == QStringLiteral("streamed translation"),
            "translation Reset should restore the exact successful result");

    session.replaceTextWithoutHistory(QStringLiteral("partial retry"));
    session.establishHistory(QStringLiteral("partial retry"));
    require(session.text() == QStringLiteral("partial retry") && !session.canUndo() &&
                session.originalText() == QStringLiteral("streamed translation"),
            "a failed partial stream should be editable without replacing the last successful baseline");
    require(session.replaceText(QStringLiteral("partial retry fixed")) && session.canUndo(),
            "a partial translation should gain independent edit history after streaming stops");
}

void tableDocumentPreservesSpansAndExportsCoveredCoordinates() {
    const ScreenshotTableDocument document = ScreenshotTableDocument::fromHtml(QStringLiteral(
        "<table><tr><th rowspan=\"2\">Group</th><th colspan=\"2\">Values</th></tr>"
        "<tr><th>A</th><th>B</th></tr><tr><td>Item</td><td>1</td><td>2</td></tr></table>"));
    require(document.rowCount() == 3 && document.columnCount() == 3,
            "table parser should preserve the recognized row and column dimensions");
    require(document.firstRowIsHeader() && document.cellText(1, 0) == QStringLiteral("Group"),
            "covered coordinates should resolve to their anchor cell");
    require(document.spanRangeAt(1, 0) == ScreenshotTableRange{0, 0, 1, 0} &&
                document.spanRangeAt(0, 2) == ScreenshotTableRange{0, 1, 0, 2},
            "rowspan and colspan geometry should be represented explicitly");
    require(document.toPlainText() == QStringLiteral("Group\tValues\t\n\tA\tB\nItem\t1\t2"),
            "TSV export should leave covered coordinates empty");

    const QString html = document.toHtml();
    require(html.contains(QStringLiteral("rowspan=\"2\"")) &&
                html.contains(QStringLiteral("colspan=\"2\"")),
            "HTML export should preserve multi-row and multi-column spans");
    const ScreenshotTableDocument roundTrip = ScreenshotTableDocument::fromHtml(html);
    require(roundTrip == document, "span-aware table HTML should round-trip without loss");
}

void tableDocumentMergeSplitClearAndValidationPolicies() {
    ScreenshotTableDocument document =
        ScreenshotTableDocument::fromPlainText(QStringLiteral("A\tB\nC\tD"));
    require(document.canMerge(ScreenshotTableRange{0, 0, 1, 1}) &&
                document.merge(ScreenshotTableRange{0, 0, 1, 1}),
            "a rectangular multi-cell range should merge");
    const ScreenshotTableCell* merged = document.anchorCellAt(0, 0);
    require(merged != nullptr && merged->rowSpan == 2 && merged->columnSpan == 2 &&
                merged->text == QStringLiteral("A\nB\nC\nD"),
            "merge should join non-empty values in row-major order");
    require(!document.isAnchor(1, 1) && document.toPlainText().endsWith(QStringLiteral("\t")),
            "covered merged coordinates should remain empty in TSV output");
    require(document.canSplit(ScreenshotTableRange{1, 1, 1, 1}) &&
                document.split(ScreenshotTableRange{1, 1, 1, 1}),
            "selecting any covered coordinate should split its merged anchor");
    require(document.cellText(0, 0) == QStringLiteral("A\nB\nC\nD") &&
                document.cellText(0, 1).isEmpty() && document.cellText(1, 0).isEmpty() &&
                document.cellText(1, 1).isEmpty(),
            "split should retain merged text only in the top-left cell");
    require(document.setCellText(1, 1, QStringLiteral("<edited> & value")) &&
                document.toHtml().contains(QStringLiteral("&lt;edited&gt; &amp; value")),
            "cell edits should be safely escaped in HTML");
    require(document.clear(ScreenshotTableRange{0, 0, 1, 1}) &&
                document.toPlainText() == QStringLiteral("\t\n\t"),
            "clear should preserve fixed dimensions while emptying cell contents");
    require(!document.setCellText(-1, 0, QStringLiteral("invalid")) &&
                !document.merge(ScreenshotTableRange{0, 0, 0, 0}) &&
                !document.split(ScreenshotTableRange{0, 0, 0, 0}) &&
                !document.clear(ScreenshotTableRange{}),
            "invalid and inapplicable edits should be rejected without mutation");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    textSelectionFollowsMouseCharacterRange();
    lineSelectionPreservesSupplementaryCharacters();
    verticalLineHitTestingFollowsTopToBottomGraphemes();
    preparedGeometryAndSelectionRevisionsAvoidRepeatedWork();
    sourceComposerUsesOnlyCapturedDisplayPixels();
    textTransformsUseOriginalPunctuationAndLineRules();
    textEditingHistoryPreservesNativeEditsAndAtomicTransforms();
    textEditingTransformsAccumulateAndManualEditsClearTheirState();
    textEditingHistoryInvalidatesRedoBranchesAndSurvivesReentry();
    textEditingHistoryIgnoresEditorLayoutFormatting();
    translationHistoryEstablishesSuccessfulAndPartialBaselines();
    tableDocumentPreservesSpansAndExportsCoveredCoordinates();
    tableDocumentMergeSplitClearAndValidationPolicies();
    return 0;
}
