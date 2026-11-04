#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QApplication>
#include <QFontDatabase>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QPolygonF quad(const QRectF& rect) {
    return {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
}

QFont textFont() {
    QFont font = QApplication::font();
    font.setPixelSize(32);
    return font;
}

void bidiRangesPreserveUnselectedVisualGaps() {
    QTextLayout layout(QStringLiteral("abc \u05d0\u05d1\u05d2 def"), textFont());
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(800);
    layout.endLayout();
    const auto fragments = screenshotTextLayoutAnnotationFragments(layout, 0, 5);
    if (fragments.size() != 2) {
        std::cerr << "Bidi fragments: " << fragments.size() << '\n';
        for (const auto& fragment : fragments)
            std::cerr << fragment.rect.left() << ".." << fragment.rect.right() << '\n';
        for (int index = 0; index < layout.text().size(); ++index)
            std::cerr << index << ": " << line.cursorToX(index, QTextLine::Leading) << " / "
                      << line.cursorToX(index, QTextLine::Trailing) << '\n';
    }
    require(fragments.size() == 2,
            "logical selection across a bidi boundary must retain its unselected visual gap");
    require(fragments[0].rect.right() < fragments[1].rect.left(),
            "bidi fragments must not fill the visually intervening unselected characters");
    for (const auto& fragment : fragments)
        require(fragment.range.start == 0 && fragment.range.end == 5,
                "bidi geometry must stay within its requested logical interval");
}

void shapedCursorBoundariesPreserveCombiningAndSurrogateClusters() {
    QTextLayout layout(QStringLiteral("A e\u0301 \U0001f600 Z"), textFont());
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(800);
    layout.endLayout();
    for (const auto& positions : {QPair<int, int>(2, 4), QPair<int, int>(5, 7)}) {
        const auto complete =
            screenshotTextLayoutAnnotationFragments(layout, positions.first, positions.second);
        const auto partial =
            screenshotTextLayoutAnnotationFragments(layout, positions.first + 1, positions.second);
        const auto shortEnd =
            screenshotTextLayoutAnnotationFragments(layout, positions.first, positions.second - 1);
        require(complete.size() == 1 && partial.size() == 1 && shortEnd.size() == 1 &&
                    complete.first().rect == partial.first().rect &&
                    complete.first().rect == shortEnd.first().rect &&
                    partial.first().range.start == positions.first &&
                    shortEnd.first().range.end == positions.second,
                "shaped cursor boundaries must keep combining and surrogate clusters intact");
    }
    QTextDocument document;
    document.setDefaultFont(textFont());
    document.setPlainText(layout.text());
    const auto documentFragments =
        screenshotFormattedTextAnnotationFragments(document, {{0, 2, 4}});
    require(documentFragments.size() == 1 && documentFragments.front().rect.width() > 0,
            "document-backed QTextLayouts must use their block's authoritative text");
}

void sourceRowsAndVerticalTextUseOrientedGeometry() {
    ScreenshotOcrPresentation presentation;
    presentation.selection = QRect(0, 0, 360, 160);
    ScreenshotOcrLine line;
    line.text = QString(31, QChar(0x7530));
    line.paragraph = true;
    line.quad = quad(QRectF(10, 10, 300, 96));
    for (const QRectF& row :
         {QRectF(10, 10, 300, 24), QRectF(10, 46, 300, 24), QRectF(10, 82, 180, 24)})
        line.sourceLineQuads.push_back(quad(row));
    presentation.lines.push_back(line);
    auto fragments = screenshotOcrTextAnnotationFragments(presentation, {{0, 0, 31}}, textFont());
    require(fragments.size() == 3, "a merged paragraph must annotate each source row separately");
    for (int index = 0; index < fragments.size(); ++index) {
        const auto bounds = fragments[index].localToSource.mapRect(fragments[index].rect);
        const auto source = line.sourceLineQuads[index].boundingRect();
        require(source.adjusted(-0.1, -0.1, 0.1, 0.1).contains(bounds),
                "source-row annotation geometry must respect the original row clipping");
    }
    for (int angle : {90, 180, 270}) {
        QTransform rotation;
        rotation.rotate(angle);
        ScreenshotOcrPresentation rotatedPresentation = presentation;
        auto& rotatedLine = rotatedPresentation.lines[0];
        rotatedLine.quad = rotation.map(rotatedLine.quad);
        for (auto& row : rotatedLine.sourceLineQuads)
            row = rotation.map(row);
        ScreenshotOcrTextLayout rotatedLayout;
        QTransform textToSource;
        require(configureScreenshotOcrTextLayout(rotatedLayout, rotatedLine, {}, textFont(),
                                                 Qt::black, {}, &textToSource) &&
                    rotatedLayout.usesSourceRows(),
                "rotated merged OCR paragraphs must retain their original source-row layout");
        const auto rotatedFragments =
            screenshotOcrTextAnnotationFragments(rotatedPresentation, {{0, 0, 31}}, textFont());
        require(rotatedFragments.size() == fragments.size(),
                "rotation must preserve the number of selected visual source rows");
        for (int index = 0; index < fragments.size(); ++index) {
            const auto expected =
                (fragments[index].localToSource * rotation).mapRect(fragments[index].rect);
            const auto actual =
                rotatedFragments[index].localToSource.mapRect(rotatedFragments[index].rect);
            require(std::abs(expected.x() - actual.x()) < 0.01 &&
                        std::abs(expected.y() - actual.y()) < 0.01 &&
                        std::abs(expected.width() - actual.width()) < 0.01 &&
                        std::abs(expected.height() - actual.height()) < 0.01,
                    "each annotation source row must rotate with its selected text");
        }
    }
    presentation.lines[0].paragraph = false;
    presentation.lines[0].sourceLineQuads.clear();
    presentation.lines[0].text = QStringLiteral("\u7530\u7530\u7530\u7530");
    presentation.lines[0].direction = ScreenshotOcrTextDirection::Vertical;
    presentation.lines[0].quad = quad(QRectF(20, 10, 30, 130));
    fragments = screenshotOcrTextAnnotationFragments(presentation, {{0, 1, 3}}, textFont());
    require(fragments.size() == 1, "vertical selection must produce one oriented column segment");
    const auto& vertical = fragments.first();
    const QPointF advance =
        vertical.localToSource.map(QPointF(1, 0)) - vertical.localToSource.map(QPointF());
    require(advance.y() > 0 && std::abs(advance.x()) < 0.001,
            "the local advance of vertical annotations must follow the text direction");
    const QRectF bounds = vertical.localToSource.mapRect(vertical.rect);
    require(bounds.height() > bounds.width() && bounds.top() > 10 && bounds.bottom() < 140,
            "a partial vertical selection must only cover its selected glyph cells");
    QTransform rotation;
    rotation.rotate(90);
    presentation.lines[0].quad = rotation.map(presentation.lines[0].quad);
    const auto rotated =
        screenshotOcrTextAnnotationFragments(presentation, {{0, 1, 3}}, textFont());
    const QRectF rotatedBounds = screenshotTextAnnotationBounds(rotated);
    require(std::abs(rotatedBounds.width() - bounds.height()) < 0.001 &&
                std::abs(rotatedBounds.height() - bounds.width()) < 0.001,
            "rotating source text must rotate its annotation segment with it");
    presentation.lines[0] = line;
    presentation.lines[0].sourceLineQuads[0].removeLast();
    const auto fallback =
        screenshotOcrTextAnnotationFragments(presentation, {{0, 0, 31}}, textFont());
    require(!fallback.isEmpty() && !screenshotTextAnnotationBounds(fallback).isEmpty(),
            "invalid source-row geometry must fall back to the bounded paragraph layout");
}

void formattedGeometryPreservesDocumentAndWrapping() {
    QTextDocument document;
    document.setDefaultFont(textFont());
    document.setDocumentMargin(0);
    document.setHtml(QStringLiteral("<p><b>Selected</b> wrapped text with several words</p>"
                                    "<table><tr><td>Cell text</td></tr></table>"));
    document.setTextWidth(180);
    const QString html = document.toHtml();
    const auto all = screenshotFormattedTextAnnotationFragments(
        document, {{0, 0, document.characterCount() - 1}});
    require(all.size() >= 4, "formatted range geometry must follow wrapped lines and table cells");
    const auto partial = screenshotFormattedTextAnnotationFragments(document, {{0, 1, 5}});
    require(!partial.isEmpty() && screenshotTextAnnotationBounds(all).contains(
                                      screenshotTextAnnotationBounds(partial)),
            "a formatted subrange must stay within the document's full annotation geometry");
    require(document.toHtml() == html,
            "extracting formatted geometry must preserve original rich-text clipboard styles");
    for (const auto& fragment : partial)
        require(fragment.range.start >= 1 && fragment.range.end <= 5,
                "formatted geometry must preserve requested UTF-16 boundaries");
    const auto firstFingerprint = screenshotFormattedTextAnnotationFingerprint(document);
    document.setHtml(html);
    document.setTextWidth(180);
    require(screenshotFormattedTextAnnotationFingerprint(document) == firstFingerprint,
            "restoring the same document must restore the same source binding");
    document.setPlainText(QStringLiteral("Replacement text"));
    require(screenshotFormattedTextAnnotationFingerprint(document) != firstFingerprint,
            "source text replacement must invalidate persisted annotation ranges");
}

void sourceFingerprintsIgnoreSelection() {
    ScreenshotOcrPresentation presentation;
    presentation.selection = QRect(0, 0, 300, 50);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Selectable e\u0301 text");
    line.quad = quad(QRectF(10, 10, 270, 30));
    presentation.lines.push_back(line);
    presentation.prepareForRendering();
    const auto fingerprint = screenshotOcrTextAnnotationFingerprint(presentation);
    presentation.selectAll();
    require(screenshotOcrTextAnnotationFingerprint(presentation) == fingerprint,
            "selection state must not participate in persistent source identity");
    presentation.lines[0].direction = ScreenshotOcrTextDirection::Vertical;
    require(screenshotOcrTextAnnotationFingerprint(presentation) != fingerprint,
            "OCR orientation changes must invalidate source geometry binding");
}

void structuralSelectionsHaveBoundsWithoutAnnotationInk() {
    QTextDocument document;
    document.setDefaultFont(textFont());
    document.setPlainText(QStringLiteral("First line\n\nLast line"));
    const QVector<ScreenshotTextSelectionRange> emptyParagraph{{0, 11, 12}};
    const auto bounds = screenshotFormattedTextSelectionBounds(document, emptyParagraph);
    require(!bounds.isEmpty() && bounds.width() == 1.0 && bounds.height() > 0,
            "a selected empty paragraph must provide caret-width popover bounds");
    require(screenshotFormattedTextAnnotationFragments(document, emptyParagraph).isEmpty(),
            "an empty paragraph selection must not annotate unrelated text");
    const QVector<ScreenshotTextSelectionRange> newline{{0, 10, 11}};
    require(!screenshotFormattedTextSelectionBounds(document, newline).isEmpty() &&
                screenshotFormattedTextAnnotationFragments(document, newline).isEmpty(),
            "a selected paragraph separator must anchor UI without generating annotation ink");
    document.setPlainText(QStringLiteral("   "));
    require(!screenshotFormattedTextSelectionBounds(document, {{0, 0, 3}}).isEmpty(),
            "nonempty whitespace selection must retain its popover anchor");
    document.setPlainText(QStringLiteral("A long first row with a short ending"));
    document.setTextWidth(180);
    const QVector<ScreenshotTextSelectionRange> wrapped{{0, 0, document.characterCount() - 1}};
    const auto fragments = screenshotFormattedTextAnnotationFragments(document, wrapped);
    QTransform rotation;
    rotation.rotate(35);
    const QRectF mappedBounds = screenshotTextAnnotationBounds(fragments, rotation);
    require(screenshotFormattedTextSelectionBounds(document, wrapped, rotation) == mappedBounds,
            "rotated selection bounds must union mapped rows without adding empty outer corners");
}

void longWrappedSelectionsKeepLocalRowsAndTabAdvance() {
    const QString text = QStringLiteral("Wrapped annotation text without tabs. ").repeated(300) +
                         QStringLiteral("\tending");
    QTextLayout layout(text, textFont());
    layout.beginLayout();
    qreal y = 0;
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(260);
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();
    require(layout.lineCount() > 300, "long wrapped fixture spans many visual rows");
    const auto row = layout.lineAt(layout.lineCount() - 3);
    const int start = row.textStart() + 1;
    const int end = row.textStart() + row.textLength() - 1;
    const auto fragments = screenshotTextLayoutAnnotationFragments(layout, start, end);
    require(!fragments.isEmpty(), "a late partial row retains its annotation geometry");
    for (const auto& fragment : fragments)
        require(fragment.range.start == start && fragment.range.end == end &&
                    fragment.rect.top() == row.y() && fragment.rect.height() == row.height(),
                "partial wrapped ranges include only their selected visual row");
    const int tab = static_cast<int>(text.indexOf(QLatin1Char('\t')));
    const auto tabLine = layout.lineForTextPosition(tab);
    const auto tabs = screenshotTextLayoutAnnotationFragments(layout, tab, tab + 1);
    const qreal first = tabLine.cursorToX(tab);
    const qreal last = tabLine.cursorToX(tab + 1);
    require(tabs.size() == 1 &&
                tabs.front().rect == QRectF(std::min(first, last), tabLine.y(),
                                            std::abs(last - first), tabLine.height()),
            "bounded row scans preserve the complete native tab selection advance");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load the Windows text-geometry fixture font");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    bidiRangesPreserveUnselectedVisualGaps();
    shapedCursorBoundariesPreserveCombiningAndSurrogateClusters();
    sourceRowsAndVerticalTextUseOrientedGeometry();
    formattedGeometryPreservesDocumentAndWrapping();
    sourceFingerprintsIgnoreSelection();
    structuralSelectionsHaveBoundsWithoutAnnotationInk();
    longWrappedSelectionsKeepLocalRowsAndTabAdvance();
    return 0;
}
