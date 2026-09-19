#include "snow_canvas_text_layout.h"

#include "snow_canvas_text.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QRawFont>
#include <QScreen>
#include <QStringList>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <cmath>
#include <limits>

namespace snow_canvas_text_layout {
namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr double kTextCursorWidth = 1.2;
constexpr double kExcalidrawBoundTextPadding = 5.0;
constexpr double kAutoResizeTextWidthSafetyEm = 0.06;
constexpr double kFallbackLogicalDpi = 96.0;

Qt::Alignment horizontalAlignmentForItem(const SnowSceneDisplayItem& item) {
    switch (item.text_horizontal_align) {
    case SNOW_TEXT_HORIZONTAL_ALIGN_CENTER:
        return Qt::AlignHCenter;
    case SNOW_TEXT_HORIZONTAL_ALIGN_RIGHT:
        return Qt::AlignRight;
    case SNOW_TEXT_HORIZONTAL_ALIGN_LEFT:
    default:
        return Qt::AlignLeft;
    }
}

double verticalTextOffsetForItem(const SnowSceneDisplayItem& item, double containerHeight,
                                 double textHeight) {
    const double available = qMax(0.0, containerHeight - textHeight);
    switch (item.text_vertical_align) {
    case SNOW_TEXT_VERTICAL_ALIGN_TOP:
        return 0.0;
    case SNOW_TEXT_VERTICAL_ALIGN_BOTTOM:
        return available;
    case SNOW_TEXT_VERTICAL_ALIGN_CENTER:
    default:
        return available / 2.0;
    }
}

QTextOption wrappedTextOptionForItem(const SnowSceneDisplayItem& item) {
    QTextOption option;
    option.setAlignment(horizontalAlignmentForItem(item));
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setUseDesignMetrics(true);
    return option;
}

QTextOption singleLineTextOption() {
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setUseDesignMetrics(true);
    return option;
}

double logicalDpiYForFontSizing() {
    if (QGuiApplication::instance() != nullptr) {
        if (const QScreen* screen = QGuiApplication::primaryScreen()) {
            const double dpi = screen->logicalDotsPerInchY();
            if (std::isfinite(dpi) && dpi > 0.0) {
                return dpi;
            }
        }
    }
    return kFallbackLogicalDpi;
}

void applyFontFamily(QFont& font, const SnowSceneDisplayItem& item) {
    const QString family = snow_canvas_text::fontFamilyFromSceneItem(item);
    if (!family.isEmpty()) {
        font.setFamily(family);
    }
}

double autoResizeTextWidthSafety(const QFont& font) {
    const double pixelSize = font.pixelSize() > 0 ? static_cast<double>(font.pixelSize())
                                                  : qMax(1.0, QFontMetricsF(font).height());
    return qMax(1.0, pixelSize * kAutoResizeTextWidthSafetyEm);
}

void configureTextDocument(QTextDocument& document, const SnowSceneDisplayItem& item,
                           const QFont& font, double textWidth, const QString& text,
                           bool substituteEmptyText) {
    document.setDocumentMargin(0.0);
    document.setDefaultFont(font);
    document.setDefaultTextOption(wrappedTextOptionForItem(item));
    document.setPlainText(substituteEmptyText && text.isEmpty() ? QStringLiteral(" ") : text);
    document.setTextWidth(qMax(1.0, textWidth));
}

QSizeF naturalTextLayoutSize(const QString& text, const QFont& font,
                             const SnowSceneDisplayItem& item, double* outInkWidth = nullptr) {
    const QString measuredText = text.isEmpty() ? QStringLiteral(" ") : text;
    const QStringList lines = measuredText.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setUseDesignMetrics(true);

    double maxWidth = 1.0;
    for (const QString& rawLine : lines) {
        const QString lineText = rawLine.isEmpty() ? QStringLiteral(" ") : rawLine;
        QTextLayout layout(lineText, font);
        layout.setTextOption(option);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (line.isValid()) {
            line.setLineWidth(std::numeric_limits<qreal>::max() / 4.0);
            maxWidth = qMax(maxWidth, static_cast<double>(line.naturalTextWidth()));
        }
        layout.endLayout();
    }
    if (outInkWidth != nullptr) {
        *outInkWidth = maxWidth;
    }

    const double measuredWidth = qMax(1.0, maxWidth + autoResizeTextWidthSafety(font));
    // QTextLine::height() can differ substantially from QTextDocument's line
    // height for variable font faces. Painting uses QTextDocument, so it must
    // also be the height authority for the stored element rectangle.
    QTextDocument document;
    configureTextDocument(document, item, font, measuredWidth, text, true);
    return QSizeF(measuredWidth, qMax(1.0, static_cast<double>(document.size().height())));
}

// Widest laid-out line of a wrapped document, in document (unwrapped ink)
// units. The painted per-line pills span each line's natural text width, so
// this is the width of their union regardless of the text alignment.
double documentInkWidth(const QTextDocument& document) {
    double widest = 1.0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextLayout* blockLayout = block.layout();
        if (blockLayout == nullptr) {
            continue;
        }
        for (int index = 0; index < blockLayout->lineCount(); ++index) {
            const QTextLine line = blockLayout->lineAt(index);
            if (line.isValid()) {
                widest = qMax(widest, static_cast<double>(line.naturalTextWidth()));
            }
        }
    }
    return widest;
}

QRectF layoutSingleLineText(QTextLayout& layout) {
    layout.setTextOption(singleLineTextOption());
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::numeric_limits<qreal>::max() / 4.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
    if (!line.isValid()) {
        return {};
    }

    QRectF bounds = layout.boundingRect();
    if (!bounds.isValid() || bounds.isEmpty()) {
        bounds = QRectF(0.0, 0.0, qMax<qreal>(1.0, line.naturalTextWidth()),
                        qMax<qreal>(1.0, line.height()));
    }
    return bounds;
}

QRectF uniteValidBounds(const QRectF& current, const QRectF& candidate, bool* hasBounds) {
    if (hasBounds == nullptr || !candidate.isValid() || candidate.isEmpty()) {
        return current;
    }
    if (!*hasBounds) {
        *hasBounds = true;
        return candidate;
    }
    return current.united(candidate);
}

QRectF glyphRunInkBounds(const QGlyphRun& run) {
    // QGlyphRun::boundingRect() describes the logical run box, which can be
    // substantially taller than the painted glyphs for variable font faces.
    const QRawFont rawFont = run.rawFont();
    if (rawFont.isValid()) {
        const QList<quint32> glyphIndexes = run.glyphIndexes();
        const QList<QPointF> positions = run.positions();
        const qsizetype count = qMin(glyphIndexes.size(), positions.size());
        QRectF bounds;
        bool hasBounds = false;
        for (qsizetype index = 0; index < count; ++index) {
            const QRectF glyphBounds =
                rawFont.boundingRect(glyphIndexes[index]).translated(positions[index]);
            bounds = uniteValidBounds(bounds, glyphBounds, &hasBounds);
        }
        if (hasBounds) {
            return bounds;
        }
    }

    const QRectF runBounds = run.boundingRect();
    return runBounds.isValid() && !runBounds.isEmpty() ? runBounds : QRectF();
}

QRectF layoutInkBounds(const QTextLayout& layout) {
    QRectF bounds;
    bool hasBounds = false;
    const QList<QGlyphRun> glyphRuns = layout.glyphRuns();
    for (const QGlyphRun& run : glyphRuns) {
        bounds = uniteValidBounds(bounds, glyphRunInkBounds(run), &hasBounds);
    }
    return hasBounds ? bounds : QRectF();
}

qreal documentCursorX(const QTextLine& line, const QRectF& blockRect, int blockCursorPosition) {
    // QTextLine positions are block-relative even on wrapped lines, and
    // cursorToX() already includes the line's horizontal position/alignment.
    return blockRect.left() + line.cursorToX(blockCursorPosition);
}

} // namespace

FontResolution resolveFont(QFont baseFont, const SnowSceneDisplayItem& item, double zoom) {
    applyFontFamily(baseFont, item);
    const double safeZoom = qMax(0.0001, zoom);
    const double targetPixelSize =
        std::isfinite(item.font_size * safeZoom) ? qMax(1.0, item.font_size * safeZoom) : 1.0;
    const int rasterPixelSize = qMax(1, static_cast<int>(std::ceil(targetPixelSize)));
    baseFont.setPixelSize(rasterPixelSize);
    baseFont.setHintingPreference(QFont::PreferNoHinting);
    return FontResolution{
        baseFont,
        targetPixelSize / static_cast<double>(rasterPixelSize),
    };
}

QFont fontForItem(QFont baseFont, const SnowSceneDisplayItem& item, double zoom) {
    applyFontFamily(baseFont, item);
    applyFontPixelSize(baseFont, item.font_size * zoom);
    return baseFont;
}

void applyFontPixelSize(QFont& font, double pixelSize) {
    const double safePixelSize = std::isfinite(pixelSize) ? qMax(1.0, pixelSize) : 1.0;
    const double pointSize = safePixelSize * 72.0 / logicalDpiYForFontSizing();
    if (std::isfinite(pointSize) && pointSize > 0.0) {
        font.setPointSizeF(pointSize);
        font.setHintingPreference(QFont::PreferNoHinting);
    } else {
        font.setPixelSize(1);
    }
}

QSizeF measureNaturalText(const QString& text, const QFont& baseFont,
                          const SnowSceneDisplayItem& item, double zoom) {
    return measureNaturalTextLayout(text, baseFont, item, zoom).layout;
}

TextMeasuredLayout measureNaturalTextLayout(const QString& text, const QFont& baseFont,
                                            const SnowSceneDisplayItem& item, double zoom) {
    const double safeZoom = qMax(0.0001, zoom);
    const FontResolution resolution = resolveFont(baseFont, item, safeZoom);
    double inkWidth = 1.0;
    const QSizeF documentSize = naturalTextLayoutSize(text, resolution.font, item, &inkWidth);
    const double toDocument = resolution.scale / safeZoom;
    TextMeasuredLayout measured;
    measured.layout = QSizeF(qMax(1.0, documentSize.width() * toDocument),
                             qMax(1.0, documentSize.height() * toDocument));
    measured.content =
        QSizeF(qMax(1.0, inkWidth * toDocument), qMax(1.0, documentSize.height() * toDocument));
    return measured;
}

QSizeF measureWrappedText(const QString& text, const QFont& baseFont,
                          const SnowSceneDisplayItem& item, double width, double zoom) {
    return measureWrappedTextLayout(text, baseFont, item, width, zoom).layout;
}

TextMeasuredLayout measureWrappedTextLayout(const QString& text, const QFont& baseFont,
                                            const SnowSceneDisplayItem& item, double width,
                                            double zoom) {
    const double safeZoom = qMax(0.0001, zoom);
    SnowSceneDisplayItem measured = item;
    measured.width = qMax(1.0, width);
    const FontResolution resolution = resolveFont(baseFont, measured, safeZoom);
    QTextDocument document;
    configureTextDocument(document, measured, resolution.font,
                          measured.width * safeZoom / resolution.scale, text, true);
    const QSizeF size = document.size();
    const double inkWidth = documentInkWidth(document);
    const double toDocument = resolution.scale / safeZoom;
    TextMeasuredLayout result;
    result.layout =
        QSizeF(qMax(1.0, size.width() * toDocument), qMax(1.0, size.height() * toDocument));
    result.content =
        QSizeF(qMax(1.0, inkWidth * toDocument), qMax(1.0, size.height() * toDocument));
    return result;
}

double measureMinimumWrappedWidth(const QFont& baseFont, const SnowSceneDisplayItem& item,
                                  double zoom) {
    const double safeZoom = qMax(0.0001, zoom);
    const FontResolution resolution = resolveFont(baseFont, item, safeZoom);
    const double viewWidth =
        QFontMetricsF(resolution.font).horizontalAdvance(QStringLiteral(" ")) * resolution.scale +
        kExcalidrawBoundTextPadding * 2.0 * safeZoom;
    return qMax(1.0, viewWidth / safeZoom);
}

DocumentLayout createDocumentLayout(const SnowSceneDisplayItem& item, const QFont& baseFont,
                                    double zoom, const QString& text, bool substituteEmptyText) {
    DocumentLayout layout;
    layout.safeZoom = qMax(0.0001, zoom);
    layout.itemWidth = qMax(1.0, item.width * layout.safeZoom);
    layout.itemHeight = qMax(1.0, item.height * layout.safeZoom);
    layout.resolution = resolveFont(baseFont, item, layout.safeZoom);
    layout.document = std::make_unique<QTextDocument>();
    configureTextDocument(*layout.document, item, layout.resolution.font,
                          layout.itemWidth / layout.resolution.scale, text, substituteEmptyText);
    const QSizeF textSize = layout.document->size();
    layout.topOffset = verticalTextOffsetForItem(item, layout.itemHeight,
                                                 textSize.height() * layout.resolution.scale);
    return layout;
}

QTransform documentToViewTransform(const SnowSceneDisplayItem& item, const QPointF& centerView,
                                   const DocumentLayout& layout) {
    QTransform transform;
    transform.translate(centerView.x(), centerView.y());
    transform.rotate(item.rotation * kRadiansToDegrees);
    transform.translate(-layout.itemWidth / 2.0, -layout.itemHeight / 2.0 + layout.topOffset);
    transform.scale(layout.resolution.scale, layout.resolution.scale);
    return transform;
}

QRectF documentContentsRect(const DocumentLayout& layout) {
    return QRectF(0.0, 0.0, layout.itemWidth / layout.resolution.scale,
                  layout.textDocument().size().height());
}

QRectF documentRectToLocalItemRect(const QRectF& documentRect, const DocumentLayout& layout) {
    return QRectF(-layout.itemWidth / 2.0 + documentRect.left() * layout.resolution.scale,
                  -layout.itemHeight / 2.0 + layout.topOffset +
                      documentRect.top() * layout.resolution.scale,
                  documentRect.width() * layout.resolution.scale,
                  documentRect.height() * layout.resolution.scale);
}

QVector<QRectF> rangeRectsInDocument(const QTextDocument& document, int rangeStart, int rangeEnd) {
    QVector<QRectF> rects;
    if (document.documentLayout() == nullptr) {
        return rects;
    }

    const int documentEnd = qMax(0, document.characterCount() - 1);
    const int selectionStart = qBound(0, qMin(rangeStart, rangeEnd), documentEnd);
    const int selectionEnd = qBound(selectionStart, qMax(rangeStart, rangeEnd), documentEnd);
    if (selectionStart >= selectionEnd) {
        return rects;
    }

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextLayout* layout = block.layout();
        if (layout == nullptr) {
            continue;
        }
        const int blockPosition = block.position();
        const QRectF blockRect = document.documentLayout()->blockBoundingRect(block);
        for (int index = 0; index < layout->lineCount(); ++index) {
            const QTextLine line = layout->lineAt(index);
            if (!line.isValid()) {
                continue;
            }

            const int lineStart = blockPosition + line.textStart();
            const int lineEnd = lineStart + line.textLength();
            const int rangeLineStart = qMax(selectionStart, lineStart);
            const int rangeLineEnd = qMin(selectionEnd, lineEnd);
            if (rangeLineStart >= rangeLineEnd) {
                continue;
            }

            const qreal startX = documentCursorX(line, blockRect, rangeLineStart - blockPosition);
            const qreal endX = documentCursorX(line, blockRect, rangeLineEnd - blockPosition);
            const qreal left = qMin(startX, endX);
            const qreal width = qMax<qreal>(1.0, std::abs(endX - startX));
            rects.push_back(QRectF(left, blockRect.top() + line.y(), width, line.height()));
        }
    }

    return rects;
}

QRectF cursorRectInDocument(const QTextDocument& document, int cursorPosition) {
    if (document.documentLayout() == nullptr) {
        return {};
    }

    const int documentEnd = qMax(0, document.characterCount() - 1);
    const int boundedPosition = qBound(0, cursorPosition, documentEnd);
    QTextBlock block = document.findBlock(boundedPosition);
    if (!block.isValid()) {
        block = document.lastBlock();
    }
    if (!block.isValid()) {
        return {};
    }

    QTextLayout* layout = block.layout();
    const QRectF blockRect = document.documentLayout()->blockBoundingRect(block);
    if (layout == nullptr || layout->lineCount() == 0) {
        const QFontMetricsF metrics(document.defaultFont());
        return QRectF(blockRect.left(), blockRect.top(), kTextCursorWidth,
                      qMax<qreal>(1.0, metrics.height()));
    }

    const int blockPosition = block.position();
    const int relativePosition =
        qBound(0, boundedPosition - blockPosition, qMax(0, block.length() - 1));
    QTextLine targetLine = layout->lineAt(layout->lineCount() - 1);
    for (int index = 0; index < layout->lineCount(); ++index) {
        const QTextLine line = layout->lineAt(index);
        if (!line.isValid()) {
            continue;
        }
        const int lineStart = line.textStart();
        const int lineEnd = lineStart + line.textLength();
        if (relativePosition >= lineStart && relativePosition <= lineEnd) {
            targetLine = line;
            break;
        }
    }

    if (!targetLine.isValid()) {
        const QFontMetricsF metrics(document.defaultFont());
        return QRectF(blockRect.left(), blockRect.top(), kTextCursorWidth,
                      qMax<qreal>(1.0, metrics.height()));
    }

    const int lineStart = targetLine.textStart();
    const int lineEnd = lineStart + targetLine.textLength();
    const int blockCursor = qBound(lineStart, relativePosition, lineEnd);
    const qreal x = documentCursorX(targetLine, blockRect, blockCursor);
    return QRectF(x, blockRect.top() + targetLine.y(), kTextCursorWidth,
                  qMax<qreal>(1.0, targetLine.height()));
}

SingleLineLayout createSingleLineLayout(const QString& text, const QFont& baseFont,
                                        const SnowSceneDisplayItem& item, double zoom) {
    SingleLineLayout result;
    result.resolution = resolveFont(baseFont, item, zoom);
    result.layout = std::make_unique<QTextLayout>(text, result.resolution.font);
    result.layoutBounds = layoutSingleLineText(*result.layout);
    if (result.layoutBounds.isValid() && !result.layoutBounds.isEmpty()) {
        const QRectF inkBounds = layoutInkBounds(*result.layout);
        result.visualBounds =
            inkBounds.isValid() && !inkBounds.isEmpty() ? inkBounds : result.layoutBounds;
    }
    return result;
}

} // namespace snow_canvas_text_layout
