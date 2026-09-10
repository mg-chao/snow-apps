#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainter>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTextOption>
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
    const QRectF block = line.quad.boundingRect();
    if (!block.isValid() || !std::isfinite(block.width()) || !std::isfinite(block.height())) {
        return {};
    }
    QVector<QRectF> boxes;
    for (const auto& quad : line.sourceLineQuads) {
        if (quad.size() != 4 || std::any_of(quad.begin(), quad.end(), [](const QPointF& point) {
                return !std::isfinite(point.x()) || !std::isfinite(point.y());
            })) {
            return {};
        }
        const QRectF bounds = quad.boundingRect();
        if (!bounds.isValid() || !block.contains(bounds) ||
            std::abs(quad[0].y() - quad[1].y()) > bounds.height() * 0.1) {
            return {};
        }
        boxes.push_back(bounds);
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
    for (auto& row : rows) {
        row = QRectF((row.x() - block.x()) / block.width(), (row.y() - block.y()) / block.height(),
                     row.width() / block.width(), row.height() / block.height());
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
                            QTransform* outTransform) {
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

    const qreal scale = std::min(destinationWidth / sourceWidth, destinationHeight / sourceHeight);
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
    return quadTransform(fittedQuad, sourceWidth, sourceHeight, outTransform);
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
    m_layoutOrigin = {};
    m_bounds = QRectF(0, 0, aspectRatio, 1);
    return true;
}

void ScreenshotOcrTextLayout::configure(const QString& text, const QFont& font,
                                        const QColor& textColor,
                                        const ScreenshotOcrTextRange& selection,
                                        ScreenshotOcrTextDirection direction,
                                        qreal targetAspectRatio, bool paragraph,
                                        const QVector<QRectF>& sourceRows) {
    targetAspectRatio = std::max<qreal>(0.0, targetAspectRatio);
    const bool hasLayout = direction == ScreenshotOcrTextDirection::Vertical
                               ? !m_verticalGlyphs.empty() || text.isEmpty()
                               : m_layout != nullptr || usesSourceRows();
    const bool aspectRatioMatches =
        qFuzzyCompare(1.0 + m_targetAspectRatio, 1.0 + targetAspectRatio);
    if (hasLayout && m_text == text && m_font == font && m_direction == direction &&
        aspectRatioMatches && m_paragraph == paragraph && m_sourceRows == sourceRows) {
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

void ScreenshotOcrTextLayout::paint(QPainter* painter, const QColor& selectionBackground,
                                    const QColor& selectionForeground) const {
    if (painter == nullptr || m_text.isEmpty()) {
        return;
    }

    painter->save();
    painter->setClipRect(m_bounds);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setPen(m_textColor);

    if (m_direction == ScreenshotOcrTextDirection::Vertical) {
        const int selectionStart = qBound(0, m_selection.start, static_cast<int>(m_text.size()));
        const int selectionEnd = qBound(selectionStart, selectionStart + m_selection.length,
                                        static_cast<int>(m_text.size()));
        for (std::size_t index = 0; index < m_verticalGlyphs.size(); ++index) {
            const VerticalGlyph& glyph = m_verticalGlyphs.at(index);
            const int glyphEnd = glyph.textStart + glyph.textLength;
            const bool selected = selectionStart < glyphEnd && selectionEnd > glyph.textStart;
            if (selected) {
                painter->fillRect(QRectF(0.0, static_cast<qreal>(index) * m_verticalCellAdvance,
                                         m_bounds.width(), m_verticalCellAdvance),
                                  selectionBackground);
            }
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
            textFormat.format.setForeground(QBrush(selected ? selectionForeground : m_textColor));
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
            QTextLayout::FormatRange textFormat;
            textFormat.start = 0;
            textFormat.length = row.textLength;
            textFormat.format.setForeground(QBrush(m_textColor));
            formats.push_back(textFormat);
            const int start = std::max(row.textStart, m_selection.start);
            const int end =
                std::min(row.textStart + row.textLength, m_selection.start + m_selection.length);
            if (end > start) {
                QTextLayout::FormatRange selected;
                selected.start = start - row.textStart;
                selected.length = end - start;
                selected.format.setBackground(QBrush(selectionBackground));
                selected.format.setForeground(QBrush(selectionForeground));
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
    QTextLayout::FormatRange textFormat;
    textFormat.start = 0;
    textFormat.length = static_cast<int>(m_text.size());
    textFormat.format.setForeground(QBrush(m_textColor));
    formats.push_back(textFormat);
    if (!m_selection.empty()) {
        QTextLayout::FormatRange range;
        range.start = qBound(0, m_selection.start, static_cast<int>(m_text.size()));
        range.length = qBound(0, m_selection.length, static_cast<int>(m_text.size()) - range.start);
        range.format.setBackground(QBrush(selectionBackground));
        range.format.setForeground(QBrush(selectionForeground));
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
                                      QTransform* textToOutput) {
    for (const QPointF& point : line.quad) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            return false;
    }
    const QString& text = line.text;
    const QPolygonF viewQuad = mappedQuad(line.quad, canvasToOutput);
    if (text.isEmpty() || viewQuad.size() != 4 || textToOutput == nullptr) {
        return false;
    }
    const qreal lineWidth = std::max(1.0, (edgeLength(viewQuad.at(0), viewQuad.at(1)) +
                                           edgeLength(viewQuad.at(3), viewQuad.at(2))) /
                                              2.0);
    const qreal lineHeight = std::max(1.0, (edgeLength(viewQuad.at(0), viewQuad.at(3)) +
                                            edgeLength(viewQuad.at(1), viewQuad.at(2))) /
                                               2.0);
    const qreal crossAxisSize =
        line.direction == ScreenshotOcrTextDirection::Vertical ? lineWidth : lineHeight;
    font.setPixelSize(std::max(kOcrTextLayoutMinPixelSize, qCeil(crossAxisSize * 2.0)));
    font.setHintingPreference(QFont::PreferNoHinting);
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
                     line.paragraph, normalizedOcrSourceRows(line));

    const QRectF sourceBounds = layout.boundingRect();
    QTransform transform;
    // Fit in the quad's local rectangle with one uniform scale, then project
    // that centered rectangle back into the rotated or perspective quad.
    if (!aspectFitQuadTransform(layout.usesSourceRows() ? viewQuad : textFitQuad,
                                sourceBounds.width(), sourceBounds.height(), &transform)) {
        return false;
    }
    *textToOutput = transform;
    return true;
}
