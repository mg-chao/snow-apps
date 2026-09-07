#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QTextBoundaryFinder>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
bool positionBefore(const ScreenshotOcrTextPosition& first,
                    const ScreenshotOcrTextPosition& second) {
    return first.lineIndex < second.lineIndex ||
           (first.lineIndex == second.lineIndex && first.characterIndex < second.characterIndex);
}

bool positionsEqual(const ScreenshotOcrTextPosition& first,
                    const ScreenshotOcrTextPosition& second) {
    return first.lineIndex == second.lineIndex && first.characterIndex == second.characterIndex;
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
        if (boundaries.isEmpty() || boundaries.constLast() != boundary) {
            boundaries.push_back(static_cast<int>(boundary));
        }
    }
    if (boundaries.constLast() != text.size()) {
        boundaries.push_back(static_cast<int>(text.size()));
    }
    return boundaries;
}

constexpr qreal kHitTestCellSize = 64.0;
constexpr qint64 kMaximumIndexedCellsPerLine = 4096;

qint32 hitTestCellCoordinate(qreal position) {
    const qreal cell = std::floor(position / kHitTestCellSize);
    return static_cast<qint32>(qBound(static_cast<qreal>(std::numeric_limits<qint32>::min()), cell,
                                      static_cast<qreal>(std::numeric_limits<qint32>::max())));
}

quint64 hitTestCellKey(qint32 x, qint32 y) {
    return (static_cast<quint64>(static_cast<quint32>(x)) << 32) | static_cast<quint32>(y);
}

qreal normalizedLineOffset(const ScreenshotOcrLine& line, const QPointF& canvasPosition) {
    if (line.quad.size() == 4) {
        const QPolygonF normalizedQuad({
            QPointF(0.0, 0.0),
            QPointF(1.0, 0.0),
            QPointF(1.0, 1.0),
            QPointF(0.0, 1.0),
        });
        QTransform transform;
        if (QTransform::quadToQuad(line.quad, normalizedQuad, transform)) {
            const QPointF normalizedPosition = transform.map(canvasPosition);
            return line.direction == ScreenshotOcrTextDirection::Vertical ? normalizedPosition.y()
                                                                          : normalizedPosition.x();
        }
    }

    const QRectF bounds = line.quad.boundingRect();
    if (line.direction == ScreenshotOcrTextDirection::Vertical) {
        return bounds.height() > 0.0 ? (canvasPosition.y() - bounds.top()) / bounds.height() : 0.0;
    }
    return bounds.width() > 0.0 ? (canvasPosition.x() - bounds.left()) / bounds.width() : 0.0;
}
} // namespace

QColor screenshotOcrContrastingTextColor(const QColor& background) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linear(background.redF()) +
                             0.7152 * linear(background.greenF()) +
                             0.0722 * linear(background.blueF());
    return QColor((luminance + 0.05) / 0.05 >= 1.05 / (luminance + 0.05) ? Qt::black : Qt::white);
}

void ScreenshotOcrPresentation::prepareForRendering() {
    if (m_geometryPrepared && m_cachedLineGeometry.size() == lines.size()) {
        return;
    }

    m_cachedLineGeometry.clear();
    m_cachedLineGeometry.reserve(lines.size());
    m_linesByCell.clear();
    m_linesByCell.reserve(lines.size() * 2);
    m_unindexedLines.clear();

    const QPolygonF normalizedQuad({
        QPointF(0.0, 0.0),
        QPointF(1.0, 0.0),
        QPointF(1.0, 1.0),
        QPointF(0.0, 1.0),
    });
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const ScreenshotOcrLine& line = lines.at(lineIndex);
        CachedLineGeometry geometry;
        geometry.bounds = line.quad.boundingRect();
        geometry.graphemeBoundaries = graphemeBoundaries(line.text);
        geometry.hasNormalizedTransform =
            line.quad.size() == 4 &&
            QTransform::quadToQuad(line.quad, normalizedQuad, geometry.canvasToNormalized);
        m_cachedLineGeometry.push_back(std::move(geometry));

        const QRectF& bounds = m_cachedLineGeometry.constLast().bounds;
        const qint32 leftCell = hitTestCellCoordinate(bounds.left());
        const qint32 rightCell = hitTestCellCoordinate(bounds.right());
        const qint32 topCell = hitTestCellCoordinate(bounds.top());
        const qint32 bottomCell = hitTestCellCoordinate(bounds.bottom());
        const qint64 columnCount = static_cast<qint64>(rightCell) - leftCell + 1;
        const qint64 rowCount = static_cast<qint64>(bottomCell) - topCell + 1;
        if (columnCount <= 0 || rowCount <= 0 || columnCount > kMaximumIndexedCellsPerLine ||
            rowCount > kMaximumIndexedCellsPerLine ||
            columnCount * rowCount > kMaximumIndexedCellsPerLine) {
            m_unindexedLines.push_back(lineIndex);
            continue;
        }
        for (qint64 y = topCell; y <= bottomCell; ++y) {
            for (qint64 x = leftCell; x <= rightCell; ++x) {
                m_linesByCell[hitTestCellKey(static_cast<qint32>(x), static_cast<qint32>(y))]
                    .push_back(lineIndex);
            }
        }
    }
    m_geometryPrepared = true;
}

void ScreenshotOcrPresentation::setLineText(int lineIndex, const QString& text) {
    if (lineIndex < 0 || lineIndex >= lines.size() || lines.at(lineIndex).text == text) {
        return;
    }
    lines[lineIndex].text = text;
    const QVector<int> boundaries = graphemeBoundaries(text);
    if (m_geometryPrepared && m_cachedLineGeometry.size() == lines.size()) {
        m_cachedLineGeometry[lineIndex].graphemeBoundaries = boundaries;
    }
    const auto clampPosition = [lineIndex, &boundaries](ScreenshotOcrTextPosition& position) {
        if (position.lineIndex == lineIndex) {
            const auto after =
                std::upper_bound(boundaries.cbegin(), boundaries.cend(), position.characterIndex);
            position.characterIndex = after == boundaries.cbegin() ? 0 : *std::prev(after);
        }
    };
    clampPosition(m_selectionAnchor);
    clampPosition(m_selectionFocus);
    ++m_selectionRevision;
}

bool ScreenshotOcrPresentation::empty() const {
    return lines.isEmpty();
}

int ScreenshotOcrPresentation::lineAt(const QPointF& canvasPosition) const {
    if (m_geometryPrepared && m_cachedLineGeometry.size() == lines.size()) {
        int matchingLine = -1;
        const auto considerLine = [this, &canvasPosition, &matchingLine](int lineIndex) {
            if ((matchingLine < 0 || lineIndex < matchingLine) &&
                lines.at(lineIndex).quad.containsPoint(canvasPosition, Qt::OddEvenFill)) {
                matchingLine = lineIndex;
            }
        };
        const auto cell = m_linesByCell.constFind(hitTestCellKey(
            hitTestCellCoordinate(canvasPosition.x()), hitTestCellCoordinate(canvasPosition.y())));
        if (cell != m_linesByCell.cend()) {
            for (int lineIndex : cell.value()) {
                considerLine(lineIndex);
            }
        }
        for (int lineIndex : m_unindexedLines) {
            considerLine(lineIndex);
        }
        return matchingLine;
    }

    const int lineCount = static_cast<int>(lines.size());
    for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        if (lines.at(lineIndex).quad.containsPoint(canvasPosition, Qt::OddEvenFill)) {
            return lineIndex;
        }
    }
    return -1;
}

int ScreenshotOcrPresentation::closestLine(const QPointF& canvasPosition) const {
    int closest = -1;
    qreal closestDistance = std::numeric_limits<qreal>::max();
    const int lineCount = static_cast<int>(lines.size());
    for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        const QRectF bounds = m_geometryPrepared && m_cachedLineGeometry.size() == lines.size()
                                  ? m_cachedLineGeometry.at(lineIndex).bounds
                                  : lines.at(lineIndex).quad.boundingRect();
        const QPointF delta = bounds.center() - canvasPosition;
        const qreal distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < closestDistance) {
            closestDistance = distance;
            closest = lineIndex;
        }
    }
    return closest;
}

ScreenshotOcrTextPosition ScreenshotOcrPresentation::textPositionAt(const QPointF& canvasPosition,
                                                                    bool useClosestLine) const {
    int lineIndex = lineAt(canvasPosition);
    if (lineIndex < 0 && useClosestLine) {
        lineIndex = closestLine(canvasPosition);
    }
    if (lineIndex < 0 || lineIndex >= lines.size()) {
        return {};
    }

    const ScreenshotOcrLine& line = lines.at(lineIndex);
    const CachedLineGeometry* cached =
        m_geometryPrepared && m_cachedLineGeometry.size() == lines.size()
            ? &m_cachedLineGeometry.at(lineIndex)
            : nullptr;
    const QVector<int> uncachedBoundaries =
        cached == nullptr ? graphemeBoundaries(line.text) : QVector<int>();
    const QVector<int>& boundaries =
        cached != nullptr ? cached->graphemeBoundaries : uncachedBoundaries;
    const qreal lineOffset = cached != nullptr && cached->hasNormalizedTransform
                                 ? (line.direction == ScreenshotOcrTextDirection::Vertical
                                        ? cached->canvasToNormalized.map(canvasPosition).y()
                                        : cached->canvasToNormalized.map(canvasPosition).x())
                                 : normalizedLineOffset(line, canvasPosition);
    const int boundaryIndex =
        qBound(0, qRound(lineOffset * static_cast<qreal>(boundaries.size() - 1)),
               static_cast<int>(boundaries.size()) - 1);
    return ScreenshotOcrTextPosition{
        lineIndex,
        boundaries.at(boundaryIndex),
    };
}

ScreenshotOcrTextPosition
ScreenshotOcrPresentation::normalizedTextPosition(const ScreenshotOcrTextPosition& position) const {
    if (!position.valid() || position.lineIndex >= lines.size()) {
        return {};
    }
    return ScreenshotOcrTextPosition{
        position.lineIndex,
        qBound(0, position.characterIndex,
               static_cast<int>(lines.at(position.lineIndex).text.size())),
    };
}

bool ScreenshotOcrPresentation::hasTextSelection() const {
    return m_selectionAnchor.valid() && m_selectionFocus.valid() &&
           !positionsEqual(m_selectionAnchor, m_selectionFocus);
}

bool ScreenshotOcrPresentation::textSelectionActive() const {
    return m_dragging;
}

quint64 ScreenshotOcrPresentation::selectionRevision() const {
    return m_selectionRevision;
}

ScreenshotOcrTextPosition ScreenshotOcrPresentation::selectionAnchor() const {
    return m_selectionAnchor;
}

ScreenshotOcrTextPosition ScreenshotOcrPresentation::selectionFocus() const {
    return m_selectionFocus;
}

void ScreenshotOcrPresentation::clearTextSelection() {
    const bool selectionChanged = m_selectionAnchor.valid() || m_selectionFocus.valid();
    m_selectionAnchor = {};
    m_selectionFocus = {};
    m_dragging = false;
    if (selectionChanged) {
        ++m_selectionRevision;
    }
}

void ScreenshotOcrPresentation::selectAll() {
    if (lines.isEmpty()) {
        clearTextSelection();
        return;
    }
    const ScreenshotOcrTextPosition nextAnchor{0, 0};
    const ScreenshotOcrTextPosition nextFocus{
        static_cast<int>(lines.size()) - 1,
        static_cast<int>(lines.constLast().text.size()),
    };
    const bool selectionChanged = !positionsEqual(m_selectionAnchor, nextAnchor) ||
                                  !positionsEqual(m_selectionFocus, nextFocus);
    m_selectionAnchor = nextAnchor;
    m_selectionFocus = nextFocus;
    m_dragging = false;
    if (selectionChanged) {
        ++m_selectionRevision;
    }
}

void ScreenshotOcrPresentation::beginTextSelection(const QPointF& canvasPosition) {
    beginTextSelection(textPositionAt(canvasPosition));
}

void ScreenshotOcrPresentation::beginTextSelection(const ScreenshotOcrTextPosition& position) {
    const ScreenshotOcrTextPosition normalized = normalizedTextPosition(position);
    if (!normalized.valid()) {
        clearTextSelection();
        return;
    }
    const bool selectionChanged = !positionsEqual(m_selectionAnchor, normalized) ||
                                  !positionsEqual(m_selectionFocus, normalized);
    m_selectionAnchor = normalized;
    m_selectionFocus = normalized;
    m_dragging = true;
    if (selectionChanged) {
        ++m_selectionRevision;
    }
}

void ScreenshotOcrPresentation::updateTextSelection(const QPointF& canvasPosition) {
    updateTextSelection(textPositionAt(canvasPosition, true));
}

void ScreenshotOcrPresentation::updateTextSelection(const ScreenshotOcrTextPosition& position) {
    if (!m_dragging) {
        return;
    }
    const ScreenshotOcrTextPosition normalized = normalizedTextPosition(position);
    if (normalized.valid() && !positionsEqual(m_selectionFocus, normalized)) {
        m_selectionFocus = normalized;
        ++m_selectionRevision;
    }
}

void ScreenshotOcrPresentation::finishTextSelection() {
    m_dragging = false;
}

bool ScreenshotOcrPresentation::lineSelected(int lineIndex) const {
    return !textSelectionForLine(lineIndex).empty();
}

ScreenshotOcrTextRange ScreenshotOcrPresentation::textSelectionForLine(int lineIndex) const {
    if (!hasTextSelection() || lineIndex < 0 || lineIndex >= lines.size()) {
        return {};
    }

    const ScreenshotOcrTextPosition first =
        positionBefore(m_selectionFocus, m_selectionAnchor) ? m_selectionFocus : m_selectionAnchor;
    const ScreenshotOcrTextPosition last =
        positionBefore(m_selectionFocus, m_selectionAnchor) ? m_selectionAnchor : m_selectionFocus;
    if (lineIndex < first.lineIndex || lineIndex > last.lineIndex) {
        return {};
    }

    const int start = lineIndex == first.lineIndex ? first.characterIndex : 0;
    const int end = lineIndex == last.lineIndex ? last.characterIndex
                                                : static_cast<int>(lines.at(lineIndex).text.size());
    return ScreenshotOcrTextRange{start, std::max(0, end - start)};
}

QString ScreenshotOcrPresentation::selectedText() const {
    if (!hasTextSelection()) {
        return {};
    }
    const ScreenshotOcrTextPosition first =
        positionBefore(m_selectionFocus, m_selectionAnchor) ? m_selectionFocus : m_selectionAnchor;
    const ScreenshotOcrTextPosition last =
        positionBefore(m_selectionFocus, m_selectionAnchor) ? m_selectionAnchor : m_selectionFocus;
    QString text;
    for (int index = first.lineIndex; index <= last.lineIndex; ++index) {
        if (index > first.lineIndex) {
            text.append(QLatin1Char('\n'));
        }
        const ScreenshotOcrTextRange range = textSelectionForLine(index);
        text.append(lines.at(index).text.mid(range.start, range.length));
    }
    return text;
}
