#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainter>
#include <QPen>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTextOption>
#include <QStringView>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qreal kOcrTextInkSafetyMargin = 1.0;
constexpr int kOcrTextLayoutMinPixelSize = 32;
constexpr qreal kOcrTextCrossAxisScale = 0.9;
constexpr qreal kOcrSourceRowMinimumScale = 0.65;
constexpr qreal kOcrSourceRowMinimumWidthFill = 0.75;
constexpr qreal kOcrSourceRowMaximumSpacingEm = 0.5;
qreal edgeLength(const QPointF& first, const QPointF& second) {
    return std::hypot(second.x() - first.x(), second.y() - first.y());
}

qreal quadCrossAxisSize(const ScreenshotOcrLine& line) {
    if (line.quad.size() != 4)
        return 0;
    return line.direction == ScreenshotOcrTextDirection::Vertical
               ? (edgeLength(line.quad[0], line.quad[1]) + edgeLength(line.quad[3], line.quad[2])) /
                     2
               : (edgeLength(line.quad[0], line.quad[3]) + edgeLength(line.quad[1], line.quad[2])) /
                     2;
}

QPolygonF mappedQuad(const QPolygonF& quad, const QTransform& transform) {
    return transform.map(quad);
}

QRectF unitedValidBounds(const QRectF& current, const QRectF& candidate, bool* hasBounds) {
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
    // Logical run bounds include font metrics padding. Raw glyph bounds are
    // the painted ink authority, including bearings and variable-font shapes.
    const QRawFont rawFont = run.rawFont();
    if (rawFont.isValid()) {
        const QList<quint32> glyphIndexes = run.glyphIndexes();
        const QList<QPointF> positions = run.positions();
        const qsizetype count = std::min(glyphIndexes.size(), positions.size());
        QRectF bounds;
        bool hasBounds = false;
        for (qsizetype index = 0; index < count; ++index) {
            const QRectF glyphBounds =
                rawFont.boundingRect(glyphIndexes.at(index)).translated(positions.at(index));
            bounds = unitedValidBounds(bounds, glyphBounds, &hasBounds);
        }
        if (hasBounds) {
            return bounds;
        }
    }

    const QRectF runBounds = run.boundingRect();
    return runBounds.isValid() && !runBounds.isEmpty() ? runBounds : QRectF();
}

QRectF textLayoutInkBounds(const QTextLayout& layout) {
    QRectF bounds;
    bool hasBounds = false;
    for (const QGlyphRun& run : layout.glyphRuns()) {
        bounds = unitedValidBounds(bounds, glyphRunInkBounds(run), &hasBounds);
    }
    return hasBounds ? bounds : QRectF();
}

bool glyphsUseCffOutlines(const QTextLayout& layout) {
    return std::ranges::any_of(layout.glyphRuns(), [](const QGlyphRun& run) {
        const QRawFont font = run.rawFont();
        return !font.fontTable("CFF ").isEmpty() || !font.fontTable("CFF2").isEmpty();
    });
}

QVector<int> graphemeBoundaries(const QString& text) {
    QVector<int> boundaries;
    boundaries.push_back(0);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.toStart();
    while (true) {
        const qsizetype boundary = finder.toNextBoundary();
        if (boundary < 0) {
            break;
        }
        if (boundaries.constLast() != boundary) {
            boundaries.push_back(static_cast<int>(boundary));
        }
    }
    if (boundaries.constLast() != text.size()) {
        boundaries.push_back(static_cast<int>(text.size()));
    }
    return boundaries;
}

QVector<QRectF> normalizedOcrSourceRows(const ScreenshotOcrLine& line) {
    if (!line.paragraph || line.direction != ScreenshotOcrTextDirection::Horizontal ||
        line.sourceLineQuads.size() < 2 || line.quad.size() != 4) {
        return {};
    }
    const QPolygonF normalizedQuad({QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)});
    QTransform toNormalized;
    if (!QTransform::quadToQuad(line.quad, normalizedQuad, toNormalized)) {
        return {};
    }
    const QRectF block(0, 0, 1, 1);
    QVector<QRectF> boxes;
    for (const auto& quad : line.sourceLineQuads) {
        if (quad.size() != 4 || std::any_of(quad.begin(), quad.end(), [](const QPointF& point) {
                return !std::isfinite(point.x()) || !std::isfinite(point.y());
            })) {
            return {};
        }
        const auto localQuad = toNormalized.map(quad);
        const QRectF bounds = localQuad.boundingRect();
        if (!bounds.isValid() || !std::isfinite(bounds.width()) ||
            !std::isfinite(bounds.height()) ||
            !block.adjusted(-0.000001, -0.000001, 0.000001, 0.000001).contains(bounds) ||
            std::abs(localQuad[0].y() - localQuad[1].y()) > bounds.height() * 0.1) {
            return {};
        }
        boxes.push_back(bounds.intersected(block));
    }
    std::stable_sort(boxes.begin(), boxes.end(), [](const QRectF& a, const QRectF& b) {
        return a.center().y() < b.center().y();
    });
    QVector<QRectF> rows;
    for (const auto& box : boxes) {
        // OCR may split one visual row into several word boxes. Count the row only once.
        if (!rows.isEmpty() && std::abs(rows.back().center().y() - box.center().y()) <=
                                   std::min(rows.back().height(), box.height()) * 0.4) {
            rows.back() = rows.back().united(box);
        } else {
            if (!rows.isEmpty() && box.top() < rows.back().bottom()) {
                return {};
            }
            rows.push_back(box);
        }
    }
    return rows;
}

std::unique_ptr<QTextLayout> createSingleLineLayout(const QString& text, const QFont& font,
                                                    QTextLine* outLine = nullptr) {
    auto layout = std::make_unique<QTextLayout>(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setUseDesignMetrics(true);
    layout->setTextOption(option);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (line.isValid()) {
        line.setLineWidth(std::numeric_limits<qreal>::max() / 4.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout->endLayout();
    if (outLine != nullptr) {
        *outLine = line;
    }
    return layout;
}

struct MeasuredSingleLineLayout {
    std::unique_ptr<QTextLayout> layout;
    QTextLine line;
    QRectF visualBounds;
};

MeasuredSingleLineLayout createMeasuredSingleLineLayout(const QString& text, const QFont& font) {
    MeasuredSingleLineLayout measured;
    measured.layout = createSingleLineLayout(text, font, &measured.line);
    measured.visualBounds = textLayoutInkBounds(*measured.layout);
    if (!measured.visualBounds.isValid() || measured.visualBounds.isEmpty()) {
        measured.visualBounds = measured.layout->boundingRect();
    }
    if (!measured.visualBounds.isValid() || measured.visualBounds.isEmpty()) {
        measured.visualBounds = QRectF(0.0, 0.0, 1.0, 1.0);
    }
    measured.visualBounds.adjust(-kOcrTextInkSafetyMargin, -kOcrTextInkSafetyMargin,
                                 kOcrTextInkSafetyMargin, kOcrTextInkSafetyMargin);
    return measured;
}

MeasuredSingleLineLayout
createWidthExpandedSingleLineLayout(const QString& text, const QFont& font, qreal targetAspectRatio,
                                    qreal maximumSpacing = std::numeric_limits<qreal>::max()) {
    MeasuredSingleLineLayout baseline = createMeasuredSingleLineLayout(text, font);
    const qreal baselineWidth = baseline.visualBounds.width();
    const qreal baselineHeight = baseline.visualBounds.height();
    const QVector<int> boundaries = graphemeBoundaries(text);
    const int graphemeCount = std::max(0, static_cast<int>(boundaries.size()) - 1);
    if (graphemeCount < 2 || targetAspectRatio <= 0.0 || baselineHeight <= 0.0 ||
        baselineWidth / baselineHeight >= targetAspectRatio) {
        return baseline;
    }

    const qreal targetWidth = targetAspectRatio * baselineHeight;
    const qreal targetExtraWidth = targetWidth - baselineWidth;
    qreal letterSpacing = targetExtraWidth / static_cast<qreal>(graphemeCount - 1);
    qreal bestError = targetExtraWidth;
    MeasuredSingleLineLayout best = std::move(baseline);

    // Absolute letter spacing is nearly linear in Qt's text shaper, but the
    // number of positioned glyphs can differ from the grapheme count. A small
    // correction loop handles ligatures and combining sequences without
    // stretching the glyph outlines themselves.
    for (int attempt = 0; attempt < 3 && std::isfinite(letterSpacing) && letterSpacing > 0.0;
         ++attempt) {
        letterSpacing = std::min(letterSpacing, maximumSpacing);
        QFont spacedFont = font;
        spacedFont.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
        MeasuredSingleLineLayout candidate = createMeasuredSingleLineLayout(text, spacedFont);
        const qreal candidateWidth = candidate.visualBounds.width();
        const qreal candidateHeight = candidate.visualBounds.height();
        const qreal error = std::abs(candidateWidth - targetAspectRatio * candidateHeight);
        if (error < bestError) {
            bestError = error;
            best = std::move(candidate);
        }

        const qreal measuredExtraWidth = candidateWidth - baselineWidth;
        if (measuredExtraWidth <= 0.0) {
            letterSpacing *= 2.0;
            continue;
        }
        const qreal correctedSpacing = letterSpacing * targetExtraWidth / measuredExtraWidth;
        if (qFuzzyCompare(1.0 + correctedSpacing, 1.0 + letterSpacing)) {
            break;
        }
        letterSpacing = correctedSpacing;
    }
    return best;
}

MeasuredSingleLineLayout createFittedParagraphRow(const QString& text, const QFont& font,
                                                  qreal targetAspectRatio) {
    auto measured = createMeasuredSingleLineLayout(text, font);
    const auto spaces =
        std::count_if(text.begin(), text.end(), [](QChar c) { return c.isSpace(); });
    const qreal maximumSpacing = font.pixelSize() * kOcrSourceRowMaximumSpacingEm;
    if (spaces > 0) {
        const qreal extra =
            targetAspectRatio * measured.visualBounds.height() - measured.visualBounds.width();
        if (extra > 0) {
            QFont spacedFont = font;
            spacedFont.setWordSpacing(std::min(maximumSpacing, extra / static_cast<qreal>(spaces)));
            auto candidate = createMeasuredSingleLineLayout(text, spacedFont);
            if (candidate.visualBounds.width() > measured.visualBounds.width() &&
                candidate.visualBounds.width() <=
                    targetAspectRatio * candidate.visualBounds.height()) {
                measured = std::move(candidate);
            }
        }
    }
    // Prefer word spacing where possible, then distribute the remaining difference between
    // graphemes. Both are bounded; glyph outlines always retain their original proportions.
    return createWidthExpandedSingleLineLayout(text, measured.layout->font(), targetAspectRatio,
                                               maximumSpacing);
}

char32_t verticalPresentationForm(char32_t character) {
    switch (character) {
    case U',':
    case U'\uff0c':
        return U'\ufe10';
    case U'\u3001':
        return U'\ufe11';
    case U'.':
    case U'\u3002':
        return U'\ufe12';
    case U':':
    case U'\uff1a':
        return U'\ufe13';
    case U';':
    case U'\uff1b':
        return U'\ufe14';
    case U'!':
    case U'\uff01':
        return U'\ufe15';
    case U'?':
    case U'\uff1f':
        return U'\ufe16';
    case U'\u3016':
        return U'\ufe17';
    case U'\u3017':
        return U'\ufe18';
    case U'\u2026':
        return U'\ufe19';
    case U'\u2014':
        return U'\ufe31';
    case U'\u2013':
        return U'\ufe32';
    case U'_':
    case U'\uff3f':
        return U'\ufe33';
    case U'(':
    case U'\uff08':
        return U'\ufe35';
    case U')':
    case U'\uff09':
        return U'\ufe36';
    case U'{':
    case U'\uff5b':
        return U'\ufe37';
    case U'}':
    case U'\uff5d':
        return U'\ufe38';
    case U'\u3014':
        return U'\ufe39';
    case U'\u3015':
        return U'\ufe3a';
    case U'\u3010':
        return U'\ufe3b';
    case U'\u3011':
        return U'\ufe3c';
    case U'\u300a':
        return U'\ufe3d';
    case U'\u300b':
        return U'\ufe3e';
    case U'\u3008':
        return U'\ufe3f';
    case U'\u3009':
        return U'\ufe40';
    case U'\u300c':
        return U'\ufe41';
    case U'\u300d':
        return U'\ufe42';
    case U'\u300e':
        return U'\ufe43';
    case U'\u300f':
        return U'\ufe44';
    case U'[':
    case U'\uff3b':
        return U'\ufe47';
    case U']':
    case U'\uff3d':
        return U'\ufe48';
    default:
        return 0;
    }
}

QString verticalDisplayText(const QString& grapheme, bool* rotated) {
    const QList<uint> codePoints = grapheme.toUcs4();
    if (codePoints.isEmpty()) {
        if (rotated != nullptr) {
            *rotated = false;
        }
        return grapheme;
    }

    if (codePoints.size() == 1) {
        const char32_t vertical = verticalPresentationForm(codePoints.constFirst());
        if (vertical != 0) {
            if (rotated != nullptr) {
                *rotated = false;
            }
            return QString::fromUcs4(&vertical, 1);
        }
    }

    const char32_t first = codePoints.constFirst();
    const bool rotate = first <= 0x02ff || (first >= 0x0370 && first <= 0x052f) ||
                        (first >= 0x0590 && first <= 0x10ff);
    if (rotated != nullptr) {
        *rotated = rotate;
    }
    return grapheme;
}

bool quadTransform(const QPolygonF& destination, qreal width, qreal height,
                   QTransform* outTransform) {
    if (outTransform == nullptr || destination.size() != 4 || width <= 0.0 || height <= 0.0) {
        return false;
    }
    const QPolygonF source({
        QPointF(0.0, 0.0),
        QPointF(width, 0.0),
        QPointF(width, height),
        QPointF(0.0, height),
    });
    return QTransform::quadToQuad(source, destination, *outTransform);
}

QPointF interpolatePoint(const QPointF& first, const QPointF& second, qreal amount) {
    return first + (second - first) * amount;
}

QPolygonF ocrTextFitQuad(const QPolygonF& quad, ScreenshotOcrTextDirection direction) {
    if (quad.size() != 4) {
        return quad;
    }

    // Keep the text centered while reserving 10% of its cross-axis region as
    // visual breathing room. The source quad remains unchanged for OCR fills
    // and hit testing.
    const qreal inset = (1.0 - kOcrTextCrossAxisScale) / 2.0;
    if (direction == ScreenshotOcrTextDirection::Vertical) {
        return QPolygonF({
            interpolatePoint(quad.at(0), quad.at(1), inset),
            interpolatePoint(quad.at(1), quad.at(0), inset),
            interpolatePoint(quad.at(2), quad.at(3), inset),
            interpolatePoint(quad.at(3), quad.at(2), inset),
        });
    }

    return QPolygonF({
        interpolatePoint(quad.at(0), quad.at(3), inset),
        interpolatePoint(quad.at(1), quad.at(2), inset),
        interpolatePoint(quad.at(2), quad.at(1), inset),
        interpolatePoint(quad.at(3), quad.at(0), inset),
    });
}

bool aspectFitQuadTransform(const QPolygonF& destination, qreal sourceWidth, qreal sourceHeight,
                            QTransform* outTransform, qreal maximumScale, qreal* fittedScale) {
    if (destination.size() != 4 || sourceWidth <= 0.0 || sourceHeight <= 0.0 ||
        outTransform == nullptr) {
        return false;
    }

    const qreal destinationWidth = (edgeLength(destination.at(0), destination.at(1)) +
                                    edgeLength(destination.at(3), destination.at(2))) /
                                   2.0;
    const qreal destinationHeight = (edgeLength(destination.at(0), destination.at(3)) +
                                     edgeLength(destination.at(1), destination.at(2))) /
                                    2.0;
    if (destinationWidth <= 0.0 || destinationHeight <= 0.0) {
        return false;
    }

    const qreal scale =
        std::min({destinationWidth / sourceWidth, destinationHeight / sourceHeight, maximumScale});
    const qreal fittedWidth = sourceWidth * scale;
    const qreal fittedHeight = sourceHeight * scale;
    const QRectF fittedRect((destinationWidth - fittedWidth) / 2.0,
                            (destinationHeight - fittedHeight) / 2.0, fittedWidth, fittedHeight);

    QTransform destinationProjection;
    if (!quadTransform(destination, destinationWidth, destinationHeight, &destinationProjection)) {
        return false;
    }
    const QPolygonF fittedQuad = destinationProjection.map(QPolygonF({
        fittedRect.topLeft(),
        fittedRect.topRight(),
        fittedRect.bottomRight(),
        fittedRect.bottomLeft(),
    }));
    if (!quadTransform(fittedQuad, sourceWidth, sourceHeight, outTransform))
        return false;
    *fittedScale = scale;
    return true;
}

} // namespace

bool ScreenshotOcrTextLayout::fitSourceRows(qreal aspectRatio) {
    if (m_sourceRows.size() < 2 || m_text.trimmed().isEmpty()) {
        return false;
    }
    // Reserve a proportional share of the remaining text for every source row. Greedily
    // filling the early rows can leave just one word in a short final row, which cannot be
    // fitted well even though a balanced set of word boundaries would fill all source rows.
    const QFontMetricsF metrics(m_font);
    qreal remainingWidth = 0;
    for (const QRectF& row : m_sourceRows) {
        remainingWidth += row.width();
    }
    auto breaks = std::make_unique<QTextLayout>(m_text, m_font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setUseDesignMetrics(true);
    breaks->setTextOption(option);
    breaks->beginLayout();
    for (int index = 0; index < m_sourceRows.size(); ++index) {
        auto line = breaks->createLine();
        if (!line.isValid()) {
            break;
        }
        const qreal advance = metrics.horizontalAdvance(m_text.mid(line.textStart()));
        const qreal width = index + 1 == m_sourceRows.size()
                                ? advance + metrics.height()
                                : advance * m_sourceRows[index].width() / remainingWidth;
        line.setLineWidth(std::max<qreal>(1, width));
        remainingWidth -= m_sourceRows[index].width();
    }
    breaks->endLayout();
    if (breaks->lineCount() != m_sourceRows.size()) {
        return false;
    }
    const auto last = breaks->lineAt(breaks->lineCount() - 1);
    if (last.textStart() + last.textLength() != m_text.size()) {
        return false;
    }

    std::vector<MeasuredSingleLineLayout> measured;
    qreal scale = std::numeric_limits<qreal>::max();
    qreal idealScale = std::numeric_limits<qreal>::max();
    for (int index = 0; index < breaks->lineCount(); ++index) {
        const auto line = breaks->lineAt(index);
        const QString text = m_text.mid(line.textStart(), line.textLength()).trimmed();
        if (text.isEmpty()) {
            return false;
        }
        auto row = createMeasuredSingleLineLayout(text, m_font);
        const QRectF& slot = m_sourceRows[index];
        const qreal heightScale =
            slot.height() * kOcrTextCrossAxisScale / row.visualBounds.height();
        idealScale = std::min(idealScale, heightScale);
        scale =
            std::min({scale, heightScale, slot.width() * aspectRatio / row.visualBounds.width()});
        measured.push_back(std::move(row));
    }
    // Cap glyphs inside each slot without shrinking the slots or their line gaps.
    if (m_fontSizeCapRatio)
        scale = std::min(scale, *m_fontSizeCapRatio / m_font.pixelSize());
    // A substantially longer translation should reflow instead of becoming tiny in fixed rows.
    if (scale < idealScale * kOcrSourceRowMinimumScale) {
        return false;
    }
    std::vector<FittedRow> fitted;
    for (int index = 0; index < breaks->lineCount(); ++index) {
        const auto line = breaks->lineAt(index);
        const QRectF& source = m_sourceRows[index];
        const QRectF slot(source.x() * aspectRatio, source.y(), source.width() * aspectRatio,
                          source.height());
        auto& row = measured[static_cast<std::size_t>(index)];
        row = createFittedParagraphRow(row.layout->text(), m_font,
                                       slot.width() / (scale * row.visualBounds.height()));
        // Sparse/very short translations cannot reproduce the source shape with reasonable
        // spacing. Let the ordinary paragraph fitter choose a readable number of rows instead.
        if (row.visualBounds.width() * scale < slot.width() * kOcrSourceRowMinimumWidthFill ||
            row.visualBounds.width() * scale > slot.width() * 1.01) {
            return false;
        }
        int start = line.textStart();
        while (start < line.textStart() + line.textLength() && m_text[start].isSpace()) {
            ++start;
        }
        QTransform transform;
        transform.translate(slot.left(), slot.center().y() - row.visualBounds.height() * scale / 2);
        transform.scale(scale, scale);
        transform.translate(-row.visualBounds.left(), -row.visualBounds.top());
        fitted.push_back({start, static_cast<int>(row.layout->text().size()), std::move(row.layout),
                          transform, slot});
    }
    m_fittedRows = std::move(fitted);
    m_sourceRowScale = scale;
    m_layoutOrigin = {};
    m_bounds = QRectF(0, 0, aspectRatio, 1);
    return true;
}

void ScreenshotOcrTextLayout::configure(const QString& text, const QFont& font,
                                        const QColor& textColor,
                                        const ScreenshotOcrTextRange& selection,
                                        ScreenshotOcrTextDirection direction,
                                        qreal targetAspectRatio, bool paragraph,
                                        const QVector<QRectF>& sourceRows,
                                        std::optional<qreal> fontSizeCapRatio) {
    targetAspectRatio = std::max<qreal>(0.0, targetAspectRatio);
    const bool hasLayout = direction == ScreenshotOcrTextDirection::Vertical
                               ? !m_verticalGlyphs.empty() || text.isEmpty()
                               : m_layout != nullptr || usesSourceRows();
    const bool aspectRatioMatches =
        qFuzzyCompare(1.0 + m_targetAspectRatio, 1.0 + targetAspectRatio);
    if (hasLayout && m_text == text && m_font == font && m_direction == direction &&
        aspectRatioMatches && m_paragraph == paragraph && m_sourceRows == sourceRows &&
        m_fontSizeCapRatio == fontSizeCapRatio) {
        m_textColor = textColor;
        m_selection = selection;
        return;
    }

    m_text = text;
    m_font = font;
    m_textColor = textColor;
    m_selection = selection;
    m_direction = direction;
    m_targetAspectRatio = targetAspectRatio;
    m_paragraph = paragraph;
    m_sourceRows = sourceRows;
    m_fontSizeCapRatio = fontSizeCapRatio;
    m_sourceRowScale = 1.0;
    m_layout.reset();
    m_line = QTextLine();
    m_verticalGlyphs.clear();
    m_fittedRows.clear();
    m_graphemeBoundaries.clear();
    m_verticalCellAdvance = 0.0;

    if (direction == ScreenshotOcrTextDirection::Vertical) {
        m_graphemeBoundaries = graphemeBoundaries(text);
        const QFontMetricsF metrics(font);
        m_verticalCellAdvance = std::max<qreal>(1.0, metrics.height());
        qreal columnWidth = m_verticalCellAdvance;

        const int graphemeCount = std::max(0, static_cast<int>(m_graphemeBoundaries.size()) - 1);
        m_verticalGlyphs.reserve(static_cast<std::size_t>(graphemeCount));
        for (int index = 0; index < graphemeCount; ++index) {
            const int start = m_graphemeBoundaries.at(index);
            const int end = m_graphemeBoundaries.at(index + 1);
            bool rotated = false;
            const QString displayText = verticalDisplayText(text.mid(start, end - start), &rotated);
            auto glyphLayout = createSingleLineLayout(displayText, font);
            QRectF inkBounds = textLayoutInkBounds(*glyphLayout);
            if (!inkBounds.isValid() || inkBounds.isEmpty()) {
                inkBounds = glyphLayout->boundingRect();
            }
            if (!inkBounds.isValid() || inkBounds.isEmpty()) {
                inkBounds = QRectF(0.0, 0.0, 1.0, 1.0);
            }
            inkBounds.adjust(-kOcrTextInkSafetyMargin, -kOcrTextInkSafetyMargin,
                             kOcrTextInkSafetyMargin, kOcrTextInkSafetyMargin);
            const qreal orientedWidth = rotated ? inkBounds.height() : inkBounds.width();
            const qreal orientedHeight = rotated ? inkBounds.width() : inkBounds.height();
            columnWidth = std::max(columnWidth, orientedWidth);
            m_verticalCellAdvance = std::max(m_verticalCellAdvance, orientedHeight);
            m_verticalGlyphs.push_back(VerticalGlyph{
                start,
                end - start,
                rotated,
                std::move(glyphLayout),
                inkBounds,
            });
        }

        if (graphemeCount > 1 && targetAspectRatio > 0.0) {
            const qreal targetColumnHeight = columnWidth / targetAspectRatio;
            m_verticalCellAdvance = std::max(
                m_verticalCellAdvance, targetColumnHeight / static_cast<qreal>(graphemeCount));
        }

        m_layoutOrigin = {};
        m_bounds = QRectF(0.0, 0.0, std::max<qreal>(1.0, columnWidth),
                          std::max<qreal>(1.0, m_verticalCellAdvance * graphemeCount));
    } else if (paragraph && targetAspectRatio > 0) {
        // Source rows use the full quad and inset each row to preserve OCR line gaps.
        if (fitSourceRows(targetAspectRatio * kOcrTextCrossAxisScale)) {
            updateGlyphRasterCacheability();
            return;
        }
        // Font ascent/descent and unused wrap width are not painted bounds. Use glyph ink for
        // both choosing the wrap and centering it in the same quad as the paragraph background.
        const auto createLayout = [&](qreal width) {
            auto layout = std::make_unique<QTextLayout>(text, font);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            option.setUseDesignMetrics(true);
            layout->setTextOption(option);
            layout->beginLayout();
            qreal y = 0;
            while (true) {
                auto line = layout->createLine();
                if (!line.isValid())
                    break;
                line.setLineWidth(width);
                line.setPosition(QPointF(0, y));
                y += line.height();
            }
            layout->endLayout();
            return layout;
        };
        const auto visualBounds = [](const QTextLayout& layout) {
            QRectF bounds = textLayoutInkBounds(layout);
            if (!bounds.isValid() || bounds.isEmpty())
                bounds = layout.boundingRect();
            if (!bounds.isValid() || bounds.isEmpty())
                bounds = QRectF(0, 0, 1, 1);
            return bounds.adjusted(-kOcrTextInkSafetyMargin, -kOcrTextInkSafetyMargin,
                                   kOcrTextInkSafetyMargin, kOcrTextInkSafetyMargin);
        };
        const QFontMetricsF metrics(font);
        qreal low = std::max<qreal>(1, metrics.height());
        qreal high = std::max(low, metrics.horizontalAdvance(text) + metrics.height());
        qreal bestScale = 0;
        QRectF bestBounds;
        for (int iteration = 0; iteration < 20; ++iteration) {
            const qreal width = (low + high) / 2;
            auto layout = createLayout(width);
            const QRectF bounds = visualBounds(*layout);
            const qreal scale = std::min(targetAspectRatio / bounds.width(), 1.0 / bounds.height());
            if (scale > bestScale) {
                bestScale = scale;
                bestBounds = bounds;
                m_layout = std::move(layout);
            }
            if (bounds.width() / bounds.height() < targetAspectRatio)
                low = width;
            else
                high = width;
        }
        m_layoutOrigin = -bestBounds.topLeft();
        m_bounds = QRectF(QPointF(), bestBounds.size());
        m_line = m_layout->lineCount() > 0 ? m_layout->lineAt(0) : QTextLine();
    } else {
        MeasuredSingleLineLayout measured =
            createWidthExpandedSingleLineLayout(text, font, targetAspectRatio);
        m_layoutOrigin = -measured.visualBounds.topLeft();
        m_bounds = QRectF(0.0, 0.0, std::max<qreal>(1.0, measured.visualBounds.width()),
                          std::max<qreal>(1.0, measured.visualBounds.height()));
        m_line = measured.line;
        m_layout = std::move(measured.layout);
    }
    updateGlyphRasterCacheability();
}

void ScreenshotOcrTextLayout::updateGlyphRasterCacheability() {
    // FreeType's CFF stem darkening requests gamma-corrected destination blending even for
    // grayscale glyphs. A transparent foreground raster cannot reproduce an opaque backdrop.
    // Inspect the final shaped runs once, including fallback fonts, rather than on selection.
    m_glyphRasterCacheable = m_layout == nullptr || !glyphsUseCffOutlines(*m_layout);
    m_glyphRasterBounds = m_bounds;
    const auto includeOrigins = [&](const QTextLayout& layout, const QTransform& transform,
                                    const QPointF& origin = {}) {
        // QTextLine first truncates its logical drawing origin to 1/64 coordinates. Include
        // the resulting baselines as well as ink so re-centering cannot change their sign.
        const QPointF fixedOrigin(std::trunc(origin.x() * 64.0) / 64.0,
                                  std::trunc(origin.y() * 64.0) / 64.0);
        for (const auto& run : layout.glyphRuns()) {
            for (const QPointF& position : run.positions()) {
                const QPointF point = transform.map(position + fixedOrigin);
                m_glyphRasterBounds.setLeft(std::min(m_glyphRasterBounds.left(), point.x()));
                m_glyphRasterBounds.setTop(std::min(m_glyphRasterBounds.top(), point.y()));
                m_glyphRasterBounds.setRight(std::max(m_glyphRasterBounds.right(), point.x()));
                m_glyphRasterBounds.setBottom(std::max(m_glyphRasterBounds.bottom(), point.y()));
            }
        }
    };
    if (m_layout != nullptr) {
        includeOrigins(*m_layout, QTransform(), m_layoutOrigin);
    }
    for (const auto& row : m_fittedRows) {
        m_glyphRasterCacheable = m_glyphRasterCacheable && !glyphsUseCffOutlines(*row.layout);
        includeOrigins(*row.layout, row.transform);
    }
    for (std::size_t index = 0; index < m_verticalGlyphs.size(); ++index) {
        const auto& glyph = m_verticalGlyphs[index];
        m_glyphRasterCacheable = m_glyphRasterCacheable && !glyphsUseCffOutlines(*glyph.layout);
        QTransform transform;
        transform.translate(m_bounds.width() / 2.0,
                            (static_cast<qreal>(index) + 0.5) * m_verticalCellAdvance);
        if (glyph.rotated) {
            transform.rotate(90.0);
        }
        includeOrigins(*glyph.layout, transform, -glyph.inkBounds.center());
    }
}

void ScreenshotOcrTextLayout::setSelection(const ScreenshotOcrTextRange& selection) {
    if (m_selection.start == selection.start && m_selection.length == selection.length) {
        return;
    }
    m_selection = selection;
}

int ScreenshotOcrTextLayout::cursorPositionAt(const QPointF& itemPosition) const {
    if (usesSourceRows()) {
        const FittedRow* closest = &m_fittedRows.front();
        qreal distance = std::numeric_limits<qreal>::max();
        for (const auto& row : m_fittedRows) {
            const qreal delta = std::abs(row.bounds.center().y() - itemPosition.y());
            if (delta < distance) {
                distance = delta;
                closest = &row;
            }
        }
        const QPointF position = closest->transform.inverted().map(itemPosition);
        return closest->textStart + closest->layout->lineAt(0).xToCursor(
                                        position.x(), QTextLine::CursorBetweenCharacters);
    }
    if (m_direction == ScreenshotOcrTextDirection::Vertical) {
        if (m_graphemeBoundaries.isEmpty() || m_verticalCellAdvance <= 0.0) {
            return 0;
        }
        const int boundaryIndex = qBound(0, qRound(itemPosition.y() / m_verticalCellAdvance),
                                         static_cast<int>(m_graphemeBoundaries.size()) - 1);
        return m_graphemeBoundaries.at(boundaryIndex);
    }
    if (!m_line.isValid()) {
        return 0;
    }
    const qreal layoutX = itemPosition.x() - m_layoutOrigin.x();
    if (m_paragraph && m_layout != nullptr) {
        const qreal y = itemPosition.y() - m_layoutOrigin.y();
        for (int i = 0; i < m_layout->lineCount(); ++i) {
            const auto line = m_layout->lineAt(i);
            if (y < line.y() + line.height() || i + 1 == m_layout->lineCount())
                return qBound(0, line.xToCursor(layoutX, QTextLine::CursorBetweenCharacters),
                              static_cast<int>(m_text.size()));
        }
    }
    return qBound(0, m_line.xToCursor(layoutX, QTextLine::CursorBetweenCharacters),
                  static_cast<int>(m_text.size()));
}

QRectF ScreenshotOcrTextLayout::boundingRect() const {
    return m_bounds;
}

QVector<ScreenshotTextAnnotationFragment>
screenshotTextLayoutAnnotationFragments(const QTextLayout& layout, int start, int end,
                                        int textOffset) {
    return screenshotTextLayoutAnnotationFragments(layout, layout.text(), start, end, textOffset);
}

QVector<ScreenshotTextAnnotationFragment>
screenshotTextLayoutAnnotationFragments(const QTextLayout& layout, const QString& text, int start,
                                        int end, int textOffset) {
    QVector<ScreenshotTextAnnotationFragment> result;
    start = qBound(0, start, static_cast<int>(text.size()));
    end = qBound(start, end, static_cast<int>(text.size()));
    if (end <= start)
        return result;
    // QTextLayout already owns Unicode cursor attributes for this shaped line. Reuse them
    // across styled runs instead of segmenting the full source string for every interval.
    if (!layout.isValidCursorPosition(start))
        start = layout.previousCursorPosition(start);
    if (!layout.isValidCursorPosition(end))
        end = layout.nextCursorPosition(end);
    const auto firstLine = layout.lineForTextPosition(start);
    const auto lastLine = layout.lineForTextPosition(end - 1);
    if (!firstLine.isValid() || !lastLine.isValid())
        return result;
    for (int index = firstLine.lineNumber(); index <= lastLine.lineNumber(); ++index) {
        const QTextLine line = layout.lineAt(index);
        const int rowStart = std::max(start, line.textStart());
        const int rowEnd = std::min(end, line.textStart() + line.textLength());
        if (!line.isValid() || rowEnd <= rowStart)
            continue;
        QVector<QRectF> cells;
        // A bidi boundary has two cursor locations. Glyph-run bounds use Qt's native
        // selection geometry and retain the correct edge of each selected script run.
        // At least one retrieval array is required: Qt filters glyph runs that report
        // isEmpty(), even when their native selection bounding rectangle is populated.
        for (const auto& run :
             line.glyphRuns(rowStart, rowEnd - rowStart, QTextLayout::RetrieveGlyphIndexes)) {
            const QRectF bounds = run.boundingRect();
            if (std::isfinite(bounds.left()) && std::isfinite(bounds.width()) &&
                bounds.width() > 0.0001)
                cells.push_back(QRectF(bounds.left(), line.y(), bounds.width(), line.height()));
        }
        // Tabs have no glyph run. Retain their text advance while excluding inline objects
        // and structural separators, which are deliberately absent from annotation ink.
        const auto rowText = QStringView(text).sliced(rowStart, rowEnd - rowStart);
        for (qsizetype tab = rowText.indexOf(QLatin1Char('\t')); tab >= 0;
             tab = rowText.indexOf(QLatin1Char('\t'), tab + 1)) {
            const int position = rowStart + static_cast<int>(tab);
            const qreal first = line.cursorToX(position);
            const qreal last = line.cursorToX(position + 1);
            if (std::isfinite(first) && std::isfinite(last) && std::abs(last - first) > 0.0001)
                cells.push_back(
                    QRectF(std::min(first, last), line.y(), std::abs(last - first), line.height()));
        }
        std::sort(cells.begin(), cells.end(), [](const QRectF& left, const QRectF& right) {
            return left.left() < right.left();
        });
        for (const QRectF& cell : cells) {
            if (!result.isEmpty() && result.back().range.start == textOffset + rowStart &&
                result.back().range.end == textOffset + rowEnd &&
                std::abs(result.back().rect.top() - cell.top()) < 0.0001 &&
                cell.left() <= result.back().rect.right() + 0.01) {
                result.back().rect = result.back().rect.united(cell);
            } else {
                result.push_back({{0, textOffset + rowStart, textOffset + rowEnd},
                                  cell,
                                  {},
                                  line.y() + line.ascent()});
            }
        }
    }
    return result;
}

QVector<ScreenshotTextAnnotationFragment>
ScreenshotOcrTextLayout::rangeFragments(const ScreenshotOcrTextRange& range) const {
    QVector<ScreenshotTextAnnotationFragment> result;
    const int start = qBound(0, range.start, static_cast<int>(m_text.size()));
    const int end = static_cast<int>(
        qBound(qint64(start), qint64(range.start) + range.length, qint64(m_text.size())));
    if (end <= start)
        return result;
    if (m_direction == ScreenshotOcrTextDirection::Vertical) {
        int first = -1;
        int last = -1;
        for (int index = 0; index < static_cast<int>(m_verticalGlyphs.size()); ++index) {
            const auto& glyph = m_verticalGlyphs.at(static_cast<std::size_t>(index));
            if (start < glyph.textStart + glyph.textLength && end > glyph.textStart) {
                if (first < 0)
                    first = index;
                last = index;
            }
        }
        if (first >= 0) {
            // Rotation puts text advance on local x while keeping the beneath side on +y.
            QTransform orientedToLayout;
            orientedToLayout.translate(m_bounds.width(), 0.0);
            orientedToLayout.rotate(90.0);
            result.push_back({{0, start, end},
                              QRectF(first * m_verticalCellAdvance, 0.0,
                                     (last - first + 1) * m_verticalCellAdvance, m_bounds.width()),
                              orientedToLayout,
                              m_bounds.width() * 0.8});
        }
        return result;
    }
    if (usesSourceRows()) {
        for (const auto& row : m_fittedRows) {
            auto fragments = screenshotTextLayoutAnnotationFragments(
                *row.layout, std::max(0, start - row.textStart),
                std::min(row.textLength, end - row.textStart), row.textStart);
            for (auto& fragment : fragments) {
                fragment.rect =
                    fragment.rect.intersected(row.transform.inverted().mapRect(row.bounds));
                if (fragment.rect.isEmpty())
                    continue;
                fragment.localToSource *= row.transform;
                result.push_back(std::move(fragment));
            }
        }
        return result;
    }
    if (!m_layout)
        return result;
    result = screenshotTextLayoutAnnotationFragments(*m_layout, start, end);
    const auto layoutToText = QTransform::fromTranslate(m_layoutOrigin.x(), m_layoutOrigin.y());
    for (auto& fragment : result) {
        fragment.rect = fragment.rect.intersected(m_bounds.translated(-m_layoutOrigin));
        fragment.localToSource *= layoutToText;
    }
    result.removeIf([](const auto& fragment) { return fragment.rect.isEmpty(); });
    return result;
}

void ScreenshotOcrTextLayout::paint(QPainter* painter, const QColor& selectionBackground,
                                    const QColor& selectionForeground, PaintMode mode) const {
    const bool selectionOnly = mode == PaintMode::SelectionOnly;
    const bool selectionBackgroundVisible = mode != PaintMode::TextOnly;
    if (painter == nullptr || m_text.isEmpty() || (selectionOnly && m_selection.empty())) {
        return;
    }

    painter->save();
    painter->setClipRect(m_bounds, Qt::IntersectClip);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    // Visibility is a paint policy, independent of cached background colors
    // that may have chosen an opaque contrasting color during text layout.
    const QColor textColor = selectionOnly ? QColor(Qt::transparent) : m_textColor;
    const QColor selectedTextColor = selectionOnly ? QColor(Qt::transparent) : selectionForeground;
    // A transparent text brush still makes Qt position and rasterize glyphs. Selection-only
    // paints suppress text at QTextLayout and QPainter entry points, retaining just backgrounds.
    painter->setPen(selectionOnly ? QPen(Qt::NoPen) : QPen(textColor));

    if (m_direction == ScreenshotOcrTextDirection::Vertical) {
        const int selectionStart = qBound(0, m_selection.start, static_cast<int>(m_text.size()));
        const int selectionEnd = qBound(selectionStart, selectionStart + m_selection.length,
                                        static_cast<int>(m_text.size()));
        for (std::size_t index = 0; index < m_verticalGlyphs.size(); ++index) {
            const VerticalGlyph& glyph = m_verticalGlyphs.at(index);
            const int glyphEnd = glyph.textStart + glyph.textLength;
            const bool selected = selectionStart < glyphEnd && selectionEnd > glyph.textStart;
            if (selected && selectionBackgroundVisible) {
                painter->fillRect(QRectF(0.0, static_cast<qreal>(index) * m_verticalCellAdvance,
                                         m_bounds.width(), m_verticalCellAdvance),
                                  selectionBackground);
            }
        }

        if (selectionOnly) {
            painter->restore();
            return;
        }

        for (std::size_t index = 0; index < m_verticalGlyphs.size(); ++index) {
            const VerticalGlyph& glyph = m_verticalGlyphs.at(index);
            if (glyph.layout == nullptr) {
                continue;
            }
            const int glyphEnd = glyph.textStart + glyph.textLength;
            const bool selected = selectionStart < glyphEnd && selectionEnd > glyph.textStart;
            QVector<QTextLayout::FormatRange> formats;
            QTextLayout::FormatRange textFormat;
            textFormat.start = 0;
            textFormat.length = static_cast<int>(glyph.layout->text().size());
            textFormat.format.setForeground(QBrush(selected ? selectedTextColor : textColor));
            formats.push_back(textFormat);

            painter->save();
            painter->translate(m_bounds.width() / 2.0,
                               (static_cast<qreal>(index) + 0.5) * m_verticalCellAdvance);
            if (glyph.rotated) {
                painter->rotate(90.0);
            }
            glyph.layout->draw(painter, -glyph.inkBounds.center(), formats);
            painter->restore();
        }
        painter->restore();
        return;
    }

    if (usesSourceRows()) {
        for (const auto& row : m_fittedRows) {
            QVector<QTextLayout::FormatRange> formats;
            const int start = std::max(row.textStart, m_selection.start);
            const int end =
                std::min(row.textStart + row.textLength, m_selection.start + m_selection.length);
            if (end > start) {
                QTextLayout::FormatRange selected;
                selected.start = start - row.textStart;
                selected.length = end - start;
                selected.format.setBackground(QBrush(
                    selectionBackgroundVisible ? selectionBackground : QColor(Qt::transparent)));
                selected.format.setForeground(selectionOnly ? QBrush(Qt::NoBrush)
                                                            : QBrush(selectedTextColor));
                formats.push_back(selected);
            }
            painter->save();
            painter->setClipRect(row.bounds, Qt::IntersectClip);
            painter->setTransform(row.transform, true);
            row.layout->draw(painter, {}, formats);
            painter->restore();
        }
        painter->restore();
        return;
    }

    if (m_layout == nullptr) {
        painter->restore();
        return;
    }

    QVector<QTextLayout::FormatRange> formats;
    // The painter pen supplies ordinary text. Overlapping whole-line foreground selections
    // make QTextLayout skip the later selected foreground or draw glyphs twice at AA edges.
    if (!m_selection.empty()) {
        QTextLayout::FormatRange range;
        range.start = qBound(0, m_selection.start, static_cast<int>(m_text.size()));
        range.length = qBound(0, m_selection.length, static_cast<int>(m_text.size()) - range.start);
        range.format.setBackground(
            QBrush(selectionBackgroundVisible ? selectionBackground : QColor(Qt::transparent)));
        range.format.setForeground(selectionOnly ? QBrush(Qt::NoBrush) : QBrush(selectedTextColor));
        formats.push_back(range);
    }

    m_layout->draw(painter, m_layoutOrigin, formats);
    painter->restore();
}

bool configureScreenshotOcrTextLayout(ScreenshotOcrTextLayout& layout,
                                      const ScreenshotOcrLine& line,
                                      const QTransform& canvasToOutput, QFont font,
                                      const QColor& defaultTextColor,
                                      const ScreenshotOcrTextRange& selection,
                                      QTransform* textToOutput, qreal* fittedFontPixelSize) {
    if (fittedFontPixelSize)
        *fittedFontPixelSize = 0;
    if (line.fontSizeCapRatio &&
        (!std::isfinite(*line.fontSizeCapRatio) || *line.fontSizeCapRatio <= 0))
        return false;
    for (const QPointF& point : line.quad) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            return false;
    }
    const QString& text = line.text;
    const QPolygonF viewQuad = mappedQuad(line.quad, canvasToOutput);
    if (text.isEmpty() || viewQuad.size() != 4 || textToOutput == nullptr) {
        return false;
    }
    const qreal lineWidth =
        (edgeLength(viewQuad.at(0), viewQuad.at(1)) + edgeLength(viewQuad.at(3), viewQuad.at(2))) /
        2.0;
    const qreal lineHeight =
        (edgeLength(viewQuad.at(0), viewQuad.at(3)) + edgeLength(viewQuad.at(1), viewQuad.at(2))) /
        2.0;
    if (!std::isfinite(lineWidth) || !std::isfinite(lineHeight) || lineWidth <= 0 ||
        lineHeight <= 0)
        return false;
    const qreal crossAxisSize =
        line.direction == ScreenshotOcrTextDirection::Vertical ? lineWidth : lineHeight;
    font.setPixelSize(std::max(kOcrTextLayoutMinPixelSize, qCeil(crossAxisSize * 2.0)));
    font.setHintingPreference(QFont::PreferNoHinting);
    // Transformed OCR glyphs are also painted into transparent device rasters. LCD coverage
    // depends on the opaque destination and cannot be reused across selection backgrounds.
    // Keep direct widget painting and cached glyphs on the same grayscale coverage path.
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
    const QPolygonF textFitQuad = ocrTextFitQuad(viewQuad, line.direction);
    const qreal targetWidth = (edgeLength(textFitQuad.at(0), textFitQuad.at(1)) +
                               edgeLength(textFitQuad.at(3), textFitQuad.at(2))) /
                              2.0;
    const qreal targetHeight = (edgeLength(textFitQuad.at(0), textFitQuad.at(3)) +
                                edgeLength(textFitQuad.at(1), textFitQuad.at(2))) /
                               2.0;
    const qreal targetAspectRatio = targetHeight > 0.0 ? targetWidth / targetHeight : 0.0;
    const QColor textColor = line.backgroundFillColor.isValid()
                                 ? screenshotOcrContrastingTextColor(line.backgroundFillColor)
                                 : defaultTextColor;
    layout.configure(text, font, textColor, selection, line.direction, targetAspectRatio,
                     line.paragraph, normalizedOcrSourceRows(line), line.fontSizeCapRatio);

    const QRectF sourceBounds = layout.boundingRect();
    QTransform transform;
    qreal fittedScale = 0;
    // Source-row glyphs are capped inside their slots, preserving the original row positions.
    const qreal maximumScale = line.fontSizeCapRatio && !layout.usesSourceRows()
                                   ? *line.fontSizeCapRatio * crossAxisSize / layout.fontPixelSize()
                                   : std::numeric_limits<qreal>::max();
    // Fit in the quad's local rectangle with one uniform scale, then project
    // that centered rectangle back into the rotated or perspective quad.
    if (!aspectFitQuadTransform(layout.usesSourceRows() ? viewQuad : textFitQuad,
                                sourceBounds.width(), sourceBounds.height(), &transform,
                                maximumScale, &fittedScale)) {
        return false;
    }
    *textToOutput = transform;
    if (fittedFontPixelSize)
        *fittedFontPixelSize = layout.fontPixelSize() * fittedScale;
    return true;
}

void setScreenshotOcrTranslationFontCaps(QVector<ScreenshotOcrLine>& translation,
                                         const QVector<ScreenshotOcrLine>& source,
                                         const QVector<QVector<int>>& sourceIndices,
                                         const QFont& font) {
    if (translation.size() != sourceIndices.size() ||
        std::all_of(translation.cbegin(), translation.cend(), [](const ScreenshotOcrLine& line) {
            return line.fontSizeCapRatio.has_value();
        }))
        return;
    QVector<qreal> baselines(source.size(), 0);
    ScreenshotOcrTextLayout layout;
    for (int index = 0; index < source.size(); ++index) {
        auto original = source[index];
        original.fontSizeCapRatio.reset();
        QTransform transform;
        if (!configureScreenshotOcrTextLayout(layout, original, {}, font, Qt::black, {}, &transform,
                                              &baselines[index]))
            baselines[index] = 0;
    }
    for (int index = 0; index < translation.size(); ++index) {
        auto& line = translation[index];
        if (line.fontSizeCapRatio)
            continue;
        const qreal crossAxis = quadCrossAxisSize(line);
        if (!std::isfinite(crossAxis) || crossAxis <= 0)
            continue;
        QVector<qreal> sizes;
        for (const int sourceIndex : sourceIndices[index]) {
            if (sourceIndex >= 0 && sourceIndex < baselines.size() &&
                std::isfinite(baselines[sourceIndex]) && baselines[sourceIndex] > 0)
                sizes.push_back(baselines[sourceIndex]);
        }
        if (sizes.isEmpty())
            continue;
        std::sort(sizes.begin(), sizes.end());
        const auto middle = sizes.size() / 2;
        const qreal median =
            sizes.size() % 2 ? sizes[middle] : (sizes[middle - 1] + sizes[middle]) / 2;
        line.fontSizeCapRatio = median / crossAxis;
    }
}
