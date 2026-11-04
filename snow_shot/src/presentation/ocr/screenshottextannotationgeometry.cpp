#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>
#include <cmath>

QByteArray screenshotOcrTextAnnotationFingerprint(const ScreenshotOcrPresentation& presentation) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_11);
    stream << quint8(1) << presentation.selection << quint64(presentation.lines.size());
    for (const auto& line : presentation.lines)
        stream << line.text << quint8(line.direction) << line.quad << line.sourceLineQuads
               << line.paragraph;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray screenshotFormattedTextAnnotationFingerprint(QTextDocument& document) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_11);
    stream << quint8(1) << document.toHtml() << document.defaultFont() << document.textWidth();
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QVector<ScreenshotTextAnnotationFragment>
screenshotOcrTextAnnotationFragments(const ScreenshotOcrPresentation& presentation,
                                     const QVector<ScreenshotTextSelectionRange>& ranges,
                                     const QFont& font) {
    QVector<ScreenshotTextAnnotationFragment> result;
    int configuredLine = -1;
    ScreenshotOcrTextLayout layout;
    QTransform textToSource;
    for (const auto& range : ranges) {
        if (range.sourceLine < 0 || range.sourceLine >= presentation.lines.size() ||
            range.end <= range.start)
            continue;
        const auto& line = presentation.lines.at(range.sourceLine);
        const int start = qBound(0, range.start, static_cast<int>(line.text.size()));
        const int end = qBound(start, range.end, static_cast<int>(line.text.size()));
        if (end <= start)
            continue;
        if (configuredLine != range.sourceLine) {
            if (!configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {},
                                                  &textToSource))
                continue;
            configuredLine = range.sourceLine;
        }
        auto fragments = layout.rangeFragments({start, end - start});
        for (auto& fragment : fragments) {
            fragment.range.sourceLine = range.sourceLine;
            fragment.localToSource *= textToSource;
            result.push_back(std::move(fragment));
        }
    }
    return result;
}

QVector<ScreenshotTextAnnotationFragment>
screenshotFormattedTextAnnotationFragments(QTextDocument& document,
                                           const QVector<ScreenshotTextSelectionRange>& ranges) {
    QVector<ScreenshotTextAnnotationFragment> result;
    // Force layout before inspecting block QTextLayouts, including freshly restored documents.
    static_cast<void>(document.documentLayout()->documentSize());
    for (const auto& range : ranges) {
        if (range.sourceLine != 0 || range.end <= range.start)
            continue;
        const int start = qBound(0, range.start, document.characterCount() - 1);
        const int end = qBound(start, range.end, document.characterCount() - 1);
        for (QTextBlock block = document.findBlock(start);
             block.isValid() && block.position() < end; block = block.next()) {
            if (!block.layout())
                continue;
            auto fragments = screenshotTextLayoutAnnotationFragments(
                *block.layout(), block.text(), std::max(0, start - block.position()),
                std::min(block.length() - 1, end - block.position()), block.position());
            const QPointF origin = document.documentLayout()->blockBoundingRect(block).topLeft();
            const QTransform blockToDocument = QTransform::fromTranslate(origin.x(), origin.y());
            for (auto& fragment : fragments) {
                fragment.localToSource *= blockToDocument;
                result.push_back(std::move(fragment));
            }
        }
    }
    return result;
}

QRectF screenshotFormattedTextSelectionBounds(QTextDocument& document,
                                              const QVector<ScreenshotTextSelectionRange>& ranges,
                                              const QTransform& documentToOutput) {
    QRectF result = screenshotTextAnnotationBounds(
        screenshotFormattedTextAnnotationFragments(document, ranges), documentToOutput);
    const auto appendCaret = [&document, &result, &documentToOutput](const QTextBlock& block,
                                                                     int position) {
        if (!block.isValid() || !block.layout())
            return;
        const auto* layout = block.layout();
        const QPointF origin = document.documentLayout()->blockBoundingRect(block).topLeft();
        QTextLine line = layout->lineForTextPosition(position);
        if (!line.isValid() && layout->lineCount() > 0)
            line = layout->lineAt(layout->lineCount() - 1);
        const QRectF caret =
            line.isValid()
                ? QRectF(origin + QPointF(line.cursorToX(position), line.y()),
                         QSizeF(1.0, line.height()))
                : QRectF(origin, QSizeF(1.0, QFontMetricsF(document.defaultFont()).height()));
        const auto mappedCaret = documentToOutput.mapRect(caret);
        result = result.isEmpty() ? mappedCaret : result.united(mappedCaret);
    };
    for (const auto& range : ranges) {
        if (range.sourceLine != 0)
            continue;
        const int start = qBound(0, range.start, document.characterCount() - 1);
        const int end = qBound(start, range.end, document.characterCount() - 1);
        if (end <= start)
            continue;
        for (QTextBlock block = document.findBlock(start);
             block.isValid() && block.position() < end; block = block.next()) {
            const int separator = block.position() + block.length() - 1;
            if (separator >= start && separator < end)
                appendCaret(block, block.length() - 1);
        }
        if (result.isEmpty()) {
            const auto block = document.findBlock(start);
            appendCaret(block, start - block.position());
        }
    }
    return result;
}

QRectF screenshotTextAnnotationBounds(const QVector<ScreenshotTextAnnotationFragment>& fragments,
                                      const QTransform& sourceToOutput) {
    QRectF result;
    for (const auto& fragment : fragments) {
        const QRectF bounds = (fragment.localToSource * sourceToOutput).mapRect(fragment.rect);
        if (bounds.isValid() && !bounds.isEmpty())
            result = result.isEmpty() ? bounds : result.united(bounds);
    }
    return result;
}
