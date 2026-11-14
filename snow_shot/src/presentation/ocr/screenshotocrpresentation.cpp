#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QTextBoundaryFinder>
#include <QTransform>

#include <algorithm>
#include <array>
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
constexpr qint64 kMaximumRectangleQueryCells = 4096;

bool finiteRectangle(const QRectF& rectangle) {
    return std::isfinite(rectangle.left()) && std::isfinite(rectangle.top()) &&
           std::isfinite(rectangle.right()) && std::isfinite(rectangle.bottom()) &&
           std::isfinite(rectangle.width()) && std::isfinite(rectangle.height());
}

bool positiveRectangleOverlap(const QRectF& first, const QRectF& second) {
    return first.left() < second.right() && second.left() < first.right() &&
           first.top() < second.bottom() && second.top() < first.bottom();
}

bool isClosedQuad(const QPolygonF& quad) {
    return quad.size() == 5 && quad.constFirst() == quad.constLast();
}

bool quadHasCrossingEdges(const QPolygonF& quad, const QRectF& bounds) {
    if (quad.size() != 4 || !bounds.isValid() || !finiteRectangle(bounds)) {
        return false;
    }
    const qreal scale = std::max(bounds.width(), bounds.height());
    std::array<QPointF, 4> points;
    for (int index = 0; index < 4; ++index) {
        const QPointF& point = quad.at(index);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            return false;
        }
        points[static_cast<std::size_t>(index)] = (point - bounds.topLeft()) / scale;
    }
    const auto cross = [](const QPointF& first, const QPointF& second, const QPointF& point) {
        const QPointF edge = second - first;
        const QPointF offset = point - first;
        return edge.x() * offset.y() - edge.y() * offset.x();
    };
    const auto onSegment = [](const QPointF& first, const QPointF& second, const QPointF& point) {
        return point.x() >= std::min(first.x(), second.x()) &&
               point.x() <= std::max(first.x(), second.x()) &&
               point.y() >= std::min(first.y(), second.y()) &&
               point.y() <= std::max(first.y(), second.y());
    };
    const auto intersects = [&cross, &onSegment](const QPointF& firstStart, const QPointF& firstEnd,
                                                 const QPointF& secondStart,
                                                 const QPointF& secondEnd) {
        // Repeated neighboring corners describe a triangle, whose remaining edges share a vertex.
        if (firstStart == secondStart || firstStart == secondEnd || firstEnd == secondStart ||
            firstEnd == secondEnd) {
            return false;
        }
        const qreal firstStartSide = cross(secondStart, secondEnd, firstStart);
        const qreal firstEndSide = cross(secondStart, secondEnd, firstEnd);
        const qreal secondStartSide = cross(firstStart, firstEnd, secondStart);
        const qreal secondEndSide = cross(firstStart, firstEnd, secondEnd);
        const auto oppositeSides = [](qreal first, qreal second) {
            return (first < 0.0 && second > 0.0) || (first > 0.0 && second < 0.0);
        };
        return (oppositeSides(firstStartSide, firstEndSide) &&
                oppositeSides(secondStartSide, secondEndSide)) ||
               (firstStartSide == 0.0 && onSegment(secondStart, secondEnd, firstStart)) ||
               (firstEndSide == 0.0 && onSegment(secondStart, secondEnd, firstEnd)) ||
               (secondStartSide == 0.0 && onSegment(firstStart, firstEnd, secondStart)) ||
               (secondEndSide == 0.0 && onSegment(firstStart, firstEnd, secondEnd));
    };
    return intersects(points[0], points[1], points[2], points[3]) ||
           intersects(points[1], points[2], points[3], points[0]);
}

bool positiveQuadRectangleOverlap(const QPolygonF& quad, const QRectF& rectangle) {
    if (quad.size() != 4 || !rectangle.isValid() ||
        std::any_of(quad.cbegin(), quad.cend(), [](const QPointF& point) {
            return !std::isfinite(point.x()) || !std::isfinite(point.y());
        })) {
        return false;
    }
    const QRectF bounds = quad.boundingRect();
    if (!bounds.isValid() || !finiteRectangle(bounds) ||
        !positiveRectangleOverlap(bounds, rectangle)) {
        return false;
    }

    // Clip the four-point OCR polygon without allocating painter paths or polygon buffers.
    std::array<QPointF, 16> points;
    std::array<QPointF, 16> clipped;
    std::copy(quad.cbegin(), quad.cend(), points.begin());
    int count = 4;
    for (int edge = 0; edge < 4 && count > 0; ++edge) {
        const bool horizontal = edge >= 2;
        const bool minimum = (edge % 2) == 0;
        const qreal boundary = horizontal ? (minimum ? rectangle.top() : rectangle.bottom())
                                          : (minimum ? rectangle.left() : rectangle.right());
        const auto coordinate = [horizontal](const QPointF& point) {
            return horizontal ? point.y() : point.x();
        };
        const auto inside = [coordinate, minimum, boundary](const QPointF& point) {
            return minimum ? coordinate(point) >= boundary : coordinate(point) <= boundary;
        };
        int clippedCount = 0;
        QPointF previous = points[static_cast<std::size_t>(count - 1)];
        bool previousInside = inside(previous);
        for (int index = 0; index < count; ++index) {
            const QPointF current = points[static_cast<std::size_t>(index)];
            const bool currentInside = inside(current);
            if (currentInside != previousInside) {
                if (clippedCount >= static_cast<int>(clipped.size())) {
                    return false;
                }
                const qreal fraction = (boundary - coordinate(previous)) /
                                       (coordinate(current) - coordinate(previous));
                clipped[static_cast<std::size_t>(clippedCount++)] =
                    previous + (current - previous) * fraction;
            }
            if (currentInside) {
                if (clippedCount >= static_cast<int>(clipped.size())) {
                    return false;
                }
                clipped[static_cast<std::size_t>(clippedCount++)] = current;
            }
            previous = current;
            previousInside = currentInside;
        }
        points.swap(clipped);
        count = clippedCount;
    }
    if (count < 3) {
        return false;
    }
    qreal doubledArea = 0.0;
    const QPointF origin = points[0];
    for (int index = 1; index + 1 < count; ++index) {
        const QPointF first = points[static_cast<std::size_t>(index)] - origin;
        const QPointF second = points[static_cast<std::size_t>(index) + 1] - origin;
        doubledArea += first.x() * second.y() - first.y() * second.x();
    }
    return std::isfinite(doubledArea) && doubledArea != 0.0;
}

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
    const double luminance = 0.2126 * linear(static_cast<double>(background.redF())) +
                             0.7152 * linear(static_cast<double>(background.greenF())) +
                             0.0722 * linear(static_cast<double>(background.blueF()));
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
    m_wholeGeometryBoxCandidates.clear();
    m_lineGeometryBounds = {};

    const QPolygonF normalizedQuad({
        QPointF(0.0, 0.0),
        QPointF(1.0, 0.0),
        QPointF(1.0, 1.0),
        QPointF(0.0, 1.0),
    });
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        // QPolygonF(QRectF) repeats its first corner. Store the same four-corner shape expected
        // by hit testing, text layout, and preview rendering without detaching open polygons.
        if (isClosedQuad(lines.at(lineIndex).quad)) {
            lines[lineIndex].quad.removeLast();
        }
        for (qsizetype sourceIndex = 0; sourceIndex < lines.at(lineIndex).sourceLineQuads.size();
             ++sourceIndex) {
            if (isClosedQuad(lines.at(lineIndex).sourceLineQuads.at(sourceIndex))) {
                lines[lineIndex].sourceLineQuads[sourceIndex].removeLast();
            }
        }
        const ScreenshotOcrLine& line = lines.at(lineIndex);
        CachedLineGeometry geometry;
        geometry.bounds = line.quad.boundingRect();
        geometry.graphemeBoundaries = graphemeBoundaries(line.text);
        geometry.hasNormalizedTransform =
            line.quad.size() == 4 &&
            QTransform::quadToQuad(line.quad, normalizedQuad, geometry.canvasToNormalized);
        geometry.hasPositiveQuadArea = !quadHasCrossingEdges(line.quad, geometry.bounds) &&
                                       positiveQuadRectangleOverlap(line.quad, geometry.bounds);
        if (geometry.hasPositiveQuadArea) {
            m_wholeGeometryBoxCandidates.push_back(lineIndex);
            // A valid rectangle has each adjacent corner on the same horizontal or vertical edge.
            geometry.axisAlignedQuad = true;
            for (int corner = 0; corner < 4; ++corner) {
                const QPointF& first = line.quad.at(corner);
                const QPointF& second = line.quad.at((corner + 1) % 4);
                if (first == second ||
                    (first.x() != geometry.bounds.left() && first.x() != geometry.bounds.right()) ||
                    (first.y() != geometry.bounds.top() && first.y() != geometry.bounds.bottom()) ||
                    (first.x() != second.x() && first.y() != second.y())) {
                    geometry.axisAlignedQuad = false;
                    break;
                }
            }
        }
        m_cachedLineGeometry.push_back(std::move(geometry));

        const QRectF& bounds = m_cachedLineGeometry.constLast().bounds;
        if (!m_cachedLineGeometry.constLast().hasPositiveQuadArea || !bounds.isValid() ||
            !finiteRectangle(bounds)) {
            continue;
        }
        m_lineGeometryBounds =
            m_lineGeometryBounds.isValid() ? m_lineGeometryBounds.united(bounds) : bounds;
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

bool ScreenshotOcrPresentation::applyPreparedColors(const ScreenshotOcrPresentation& source) {
    if (lines.size() != source.lines.size())
        return false;
    bool changed = solidBackgroundFill != source.solidBackgroundFill;
    solidBackgroundFill = source.solidBackgroundFill;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const auto& color = source.lines.at(index);
        const auto& current = lines.at(index);
        if (current.sourceTextColor == color.sourceTextColor &&
            current.sourceLineTextColors == color.sourceLineTextColors &&
            current.backgroundFillColor == color.backgroundFillColor)
            continue;
        auto& line = lines[index];
        line.sourceTextColor = color.sourceTextColor;
        line.sourceLineTextColors = color.sourceLineTextColors;
        line.backgroundFillColor = color.backgroundFillColor;
        changed = true;
    }
    m_textColorImageKey = source.m_textColorImageKey;
    m_textColorCanvasRect = source.m_textColorCanvasRect;
    m_textColorGeometry = source.m_textColorGeometry;
    m_fillColorImageKey = source.m_fillColorImageKey;
    m_fillColorCanvasRect = source.m_fillColorCanvasRect;
    m_fillColorGeometry = source.m_fillColorGeometry;
    return changed;
}

void ScreenshotOcrPresentation::setLineText(int lineIndex, const QString& text, bool streaming) {
    if (lineIndex < 0 || lineIndex >= lines.size() ||
        (lines.at(lineIndex).text == text && lines.at(lineIndex).translationStreaming == streaming))
        return;
    const bool changedText = lines.at(lineIndex).text != text;
    lines[lineIndex].translationStreaming = streaming;
    if (!changedText)
        return;
    lines[lineIndex].text = text;
    const QVector<int> boundaries = graphemeBoundaries(text);
    if (m_geometryPrepared && m_cachedLineGeometry.size() == lines.size()) {
        m_cachedLineGeometry[lineIndex].graphemeBoundaries = boundaries;
    }
    const auto clampCharacter = [&boundaries](int characterIndex) {
        const auto after = std::upper_bound(boundaries.cbegin(), boundaries.cend(), characterIndex);
        return after == boundaries.cbegin() ? 0 : *std::prev(after);
    };
    const auto clampPosition = [lineIndex, &clampCharacter](ScreenshotOcrTextPosition& position) {
        if (position.lineIndex == lineIndex) {
            position.characterIndex = clampCharacter(position.characterIndex);
        }
    };
    clampPosition(m_selectionAnchor);
    clampPosition(m_selectionFocus);
    if (m_allTextSelected && lineIndex == m_selectionFocus.lineIndex) {
        m_selectionFocus.characterIndex = static_cast<int>(text.size());
    }
    auto selected = m_sparseSelection.find(lineIndex);
    if (selected != m_sparseSelection.end() && !selected->fullLine) {
        const int start = clampCharacter(selected->range.start);
        const int end = clampCharacter(selected->range.start + selected->range.length);
        selected->range = {start, std::max(0, end - start)};
        if (selected->range.empty()) {
            m_sparseSelection.erase(selected);
        }
    }
    beginSelectionChange();
    markSelectionLineChanged(lineIndex);
}

bool ScreenshotOcrPresentation::empty() const {
    return lines.isEmpty();
}

int ScreenshotOcrPresentation::lineAt(const QPointF& canvasPosition) const {
    if (!std::isfinite(canvasPosition.x()) || !std::isfinite(canvasPosition.y())) {
        return -1;
    }
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
    if (m_sparseSelectionActive) {
        return !m_sparseSelection.isEmpty();
    }
    return m_allTextSelected || (m_selectionAnchor.valid() && m_selectionFocus.valid() &&
                                 !positionsEqual(m_selectionAnchor, m_selectionFocus));
}

bool ScreenshotOcrPresentation::textSelectionActive() const {
    return m_dragging || m_lineBoxDragging;
}

quint64 ScreenshotOcrPresentation::selectionRevision() const {
    return m_selectionRevision;
}

quint64 ScreenshotOcrPresentation::lineSelectionRevision(int lineIndex) const {
    return lineIndex >= 0 && lineIndex < m_lineSelectionRevisions.size()
               ? m_lineSelectionRevisions.at(lineIndex)
               : 0;
}

const QVector<int>* ScreenshotOcrPresentation::selectionChangesSince(quint64 revision) const {
    return revision == m_previousSelectionRevision ? &m_changedSelectionLines : nullptr;
}

void ScreenshotOcrPresentation::beginSelectionChange() {
    m_previousSelectionRevision = m_selectionRevision;
    ++m_selectionRevision;
    m_changedSelectionLines.clear();
    m_lineSelectionRevisions.resize(lines.size());
}

void ScreenshotOcrPresentation::markSelectionLineChanged(int lineIndex) {
    if (lineIndex >= 0 && lineIndex < m_lineSelectionRevisions.size() &&
        m_lineSelectionRevisions.at(lineIndex) != m_selectionRevision) {
        m_lineSelectionRevisions[lineIndex] = m_selectionRevision;
        m_changedSelectionLines.push_back(lineIndex);
    }
}

void ScreenshotOcrPresentation::markCommittedSelectionChanged() {
    if (m_sparseSelectionActive) {
        for (auto selected = m_sparseSelection.cbegin(); selected != m_sparseSelection.cend();
             ++selected) {
            markSelectionLineChanged(selected.key());
        }
    } else if (m_selectionAnchor.valid() && m_selectionFocus.valid()) {
        const int first =
            std::max(0, std::min(m_selectionAnchor.lineIndex, m_selectionFocus.lineIndex));
        const int last =
            std::min(static_cast<int>(lines.size()) - 1,
                     std::max(m_selectionAnchor.lineIndex, m_selectionFocus.lineIndex));
        for (int index = first; index <= last; ++index) {
            markSelectionLineChanged(index);
        }
    }
}

void ScreenshotOcrPresentation::clearLineBoxPreview() {
    for (int lineIndex : m_boxCandidates) {
        m_boxPreviewActions[lineIndex] = ScreenshotOcrLineSelectionPreview::None;
        markSelectionLineChanged(lineIndex);
    }
    m_lineBoxDragging = false;
    m_lineBoxRectangle = {};
    m_boxDeselectAtPress.clear();
    m_boxCandidates.clear();
}

ScreenshotOcrTextPosition ScreenshotOcrPresentation::selectionAnchor() const {
    return m_selectionAnchor;
}

ScreenshotOcrTextPosition ScreenshotOcrPresentation::selectionFocus() const {
    return m_selectionFocus;
}

void ScreenshotOcrPresentation::clearTextSelection() {
    const bool selectionChanged = m_selectionAnchor.valid() || m_selectionFocus.valid() ||
                                  m_sparseSelectionActive || m_lineBoxDragging;
    if (selectionChanged) {
        beginSelectionChange();
        markCommittedSelectionChanged();
        clearLineBoxPreview();
    }
    m_selectionAnchor = {};
    m_selectionFocus = {};
    m_sparseSelection.clear();
    m_sparseSelectionActive = false;
    m_allTextSelected = false;
    m_dragging = false;
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
                                  !positionsEqual(m_selectionFocus, nextFocus) ||
                                  m_sparseSelectionActive || !m_allTextSelected ||
                                  m_lineBoxDragging;
    if (selectionChanged) {
        beginSelectionChange();
        markCommittedSelectionChanged();
        clearLineBoxPreview();
    }
    m_selectionAnchor = nextAnchor;
    m_selectionFocus = nextFocus;
    m_sparseSelection.clear();
    m_sparseSelectionActive = false;
    m_allTextSelected = true;
    m_dragging = false;
    if (selectionChanged) {
        markCommittedSelectionChanged();
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
                                  !positionsEqual(m_selectionFocus, normalized) ||
                                  m_sparseSelectionActive || m_allTextSelected || m_lineBoxDragging;
    if (selectionChanged) {
        beginSelectionChange();
        markCommittedSelectionChanged();
        clearLineBoxPreview();
    }
    m_selectionAnchor = normalized;
    m_selectionFocus = normalized;
    m_sparseSelection.clear();
    m_sparseSelectionActive = false;
    m_allTextSelected = false;
    m_dragging = true;
    if (selectionChanged) {
        markSelectionLineChanged(normalized.lineIndex);
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
        beginSelectionChange();
        const int first = std::min(m_selectionFocus.lineIndex, normalized.lineIndex);
        const int last = std::max(m_selectionFocus.lineIndex, normalized.lineIndex);
        for (int index = first; index <= last; ++index) {
            markSelectionLineChanged(index);
        }
        m_selectionFocus = normalized;
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

    if (m_sparseSelectionActive) {
        const auto selected = m_sparseSelection.constFind(lineIndex);
        if (selected == m_sparseSelection.cend()) {
            return {};
        }
        return selected->fullLine
                   ? ScreenshotOcrTextRange{0, static_cast<int>(lines.at(lineIndex).text.size())}
                   : selected->range;
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
    if (m_sparseSelectionActive) {
        QString text;
        bool previousLine = false;
        for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            const auto selected = m_sparseSelection.constFind(lineIndex);
            if (selected == m_sparseSelection.cend()) {
                continue;
            }
            if (previousLine) {
                text.append(QLatin1Char('\n'));
            }
            const ScreenshotOcrTextRange range = textSelectionForLine(lineIndex);
            text.append(lines.at(lineIndex).text.mid(range.start, range.length));
            previousLine = true;
        }
        return text;
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

bool ScreenshotOcrPresentation::lineFullySelected(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= lines.size()) {
        return false;
    }
    if (m_sparseSelectionActive) {
        const auto selected = m_sparseSelection.constFind(lineIndex);
        if (selected != m_sparseSelection.cend() && selected->fullLine) {
            return true;
        }
    } else if (m_allTextSelected) {
        return true;
    }
    const ScreenshotOcrTextRange range = textSelectionForLine(lineIndex);
    return !range.empty() && range.start == 0 && range.length == lines.at(lineIndex).text.size();
}

void ScreenshotOcrPresentation::materializeSparseSelection() {
    if (m_sparseSelectionActive) {
        return;
    }
    const bool hasCommittedSelection = hasTextSelection();
    const int first = hasCommittedSelection
                          ? std::min(m_selectionAnchor.lineIndex, m_selectionFocus.lineIndex)
                          : 0;
    const int last = hasCommittedSelection
                         ? std::max(m_selectionAnchor.lineIndex, m_selectionFocus.lineIndex)
                         : -1;
    m_sparseSelection.reserve(std::max(0, last - first + 1) + m_boxCandidates.size());
    for (int lineIndex = first; lineIndex <= last; ++lineIndex) {
        if (lineSelectionPreview(lineIndex) == ScreenshotOcrLineSelectionPreview::Deselect) {
            continue;
        }
        const ScreenshotOcrTextRange range = textSelectionForLine(lineIndex);
        if (!range.empty() || m_allTextSelected) {
            const bool fullLine =
                m_allTextSelected ||
                (range.start == 0 && range.length == lines.at(lineIndex).text.size());
            m_sparseSelection.insert(lineIndex, LineSelection{range, fullLine});
        }
    }
    m_sparseSelectionActive = true;
    m_allTextSelected = false;
    m_selectionAnchor = {};
    m_selectionFocus = {};
}

bool ScreenshotOcrPresentation::lineBoxSelectionActive() const {
    return m_lineBoxDragging;
}

QRectF ScreenshotOcrPresentation::lineBoxSelectionRect() const {
    return m_lineBoxRectangle;
}

ScreenshotOcrLineSelectionPreview
ScreenshotOcrPresentation::lineSelectionPreview(int lineIndex) const {
    if (!m_lineBoxDragging || lineIndex < 0 || lineIndex >= m_boxPreviewActions.size()) {
        return ScreenshotOcrLineSelectionPreview::None;
    }
    return m_boxPreviewActions.at(lineIndex);
}

void ScreenshotOcrPresentation::beginLineBoxSelection(const QPointF& origin) {
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y())) {
        return;
    }
    prepareForRendering();
    beginSelectionChange();
    clearLineBoxPreview();
    m_dragging = false;
    m_lineBoxDragging = true;
    m_lineBoxRectangle = QRectF(origin, QSizeF(0.0, 0.0));
    m_boxDeselectAtPress.resize(lines.size());
    m_boxPreviewActions.resize(lines.size());
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        m_boxDeselectAtPress[lineIndex] = lineFullySelected(lineIndex) ? 1 : 0;
    }
}

void ScreenshotOcrPresentation::findLinesIntersectingRectangle(const QRectF& rectangle) {
    m_boxCandidateScratch.clear();
    if (!rectangle.isValid() || !finiteRectangle(rectangle)) {
        return;
    }
    if (m_lineGeometryBounds.isValid() && rectangle.contains(m_lineGeometryBounds)) {
        // Geometry is immutable during a drag. Share the prepared reading-order hit vector.
        m_boxCandidateScratch = m_wholeGeometryBoxCandidates;
        return;
    }
    m_hitTestVisitRevisions.resize(lines.size());
    if (++m_hitTestVisitRevision == 0) {
        m_hitTestVisitRevisions.fill(0);
        ++m_hitTestVisitRevision;
    }
    const auto considerLine = [this, &rectangle](int lineIndex) {
        if (m_hitTestVisitRevisions.at(lineIndex) == m_hitTestVisitRevision) {
            return;
        }
        m_hitTestVisitRevisions[lineIndex] = m_hitTestVisitRevision;
        const CachedLineGeometry& geometry = m_cachedLineGeometry.at(lineIndex);
        if (geometry.hasPositiveQuadArea && positiveRectangleOverlap(geometry.bounds, rectangle) &&
            (geometry.axisAlignedQuad || rectangle.contains(geometry.bounds) ||
             positiveQuadRectangleOverlap(lines.at(lineIndex).quad, rectangle))) {
            m_boxCandidateScratch.push_back(lineIndex);
        }
    };
    const qint32 leftCell = hitTestCellCoordinate(rectangle.left());
    const qint32 rightCell = hitTestCellCoordinate(rectangle.right());
    const qint32 topCell = hitTestCellCoordinate(rectangle.top());
    const qint32 bottomCell = hitTestCellCoordinate(rectangle.bottom());
    const qint64 columnCount = static_cast<qint64>(rightCell) - leftCell + 1;
    const qint64 rowCount = static_cast<qint64>(bottomCell) - topCell + 1;
    if (columnCount > kMaximumRectangleQueryCells || rowCount > kMaximumRectangleQueryCells ||
        columnCount * rowCount > kMaximumRectangleQueryCells) {
        for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            considerLine(lineIndex);
        }
    } else {
        for (qint64 y = topCell; y <= bottomCell; ++y) {
            for (qint64 x = leftCell; x <= rightCell; ++x) {
                const auto cell = m_linesByCell.constFind(
                    hitTestCellKey(static_cast<qint32>(x), static_cast<qint32>(y)));
                if (cell != m_linesByCell.cend()) {
                    for (int lineIndex : cell.value()) {
                        considerLine(lineIndex);
                    }
                }
            }
        }
        for (int lineIndex : m_unindexedLines) {
            considerLine(lineIndex);
        }
    }
    std::sort(m_boxCandidateScratch.begin(), m_boxCandidateScratch.end());
}

void ScreenshotOcrPresentation::updateLineBoxSelection(const QRectF& rectangle) {
    if (!m_lineBoxDragging) {
        return;
    }
    const QRectF normalized = finiteRectangle(rectangle) ? rectangle.normalized() : QRectF();
    if (m_lineBoxRectangle == normalized) {
        return;
    }
    prepareForRendering();
    findLinesIntersectingRectangle(normalized);
    beginSelectionChange();
    if (m_boxCandidates == m_boxCandidateScratch) {
        m_lineBoxRectangle = normalized;
        return;
    }
    auto previous = m_boxCandidates.cbegin();
    auto next = m_boxCandidateScratch.cbegin();
    while (previous != m_boxCandidates.cend() || next != m_boxCandidateScratch.cend()) {
        if (next == m_boxCandidateScratch.cend() ||
            (previous != m_boxCandidates.cend() && *previous < *next)) {
            m_boxPreviewActions[*previous] = ScreenshotOcrLineSelectionPreview::None;
            markSelectionLineChanged(*previous++);
        } else if (previous == m_boxCandidates.cend() || *next < *previous) {
            m_boxPreviewActions[*next] = m_boxDeselectAtPress.at(*next) != 0
                                             ? ScreenshotOcrLineSelectionPreview::Deselect
                                             : ScreenshotOcrLineSelectionPreview::Select;
            markSelectionLineChanged(*next++);
        } else {
            ++previous;
            ++next;
        }
    }
    m_boxCandidates.swap(m_boxCandidateScratch);
    m_lineBoxRectangle = normalized;
}

void ScreenshotOcrPresentation::finishLineBoxSelection() {
    if (!m_lineBoxDragging) {
        return;
    }
    beginSelectionChange();
    if (!m_boxCandidates.isEmpty()) {
        const bool materialized = !m_sparseSelectionActive;
        materializeSparseSelection();
        m_sparseSelection.reserve(m_sparseSelection.size() + m_boxCandidates.size());
        for (int lineIndex : m_boxCandidates) {
            if (m_boxDeselectAtPress.at(lineIndex) != 0) {
                if (!materialized) {
                    m_sparseSelection.remove(lineIndex);
                }
            } else {
                m_sparseSelection.insert(lineIndex, LineSelection{{}, true});
            }
        }
    }
    clearLineBoxPreview();
}

void ScreenshotOcrPresentation::cancelLineBoxSelection() {
    if (!m_lineBoxDragging) {
        return;
    }
    beginSelectionChange();
    clearLineBoxPreview();
}

void ScreenshotOcrPresentation::copyCommittedSelectionFrom(
    const ScreenshotOcrPresentation& source) {
    if (&source == this) {
        cancelLineBoxSelection();
        finishTextSelection();
        return;
    }
    beginSelectionChange();
    markCommittedSelectionChanged();
    clearLineBoxPreview();
    m_selectionAnchor = normalizedTextPosition(source.m_selectionAnchor);
    m_selectionFocus = normalizedTextPosition(source.m_selectionFocus);
    m_sparseSelection = source.m_sparseSelection;
    m_sparseSelectionActive = source.m_sparseSelectionActive;
    m_allTextSelected = source.m_allTextSelected && !lines.isEmpty();
    m_dragging = false;
    for (auto selected = m_sparseSelection.begin(); selected != m_sparseSelection.end();) {
        if (selected.key() < 0 || selected.key() >= lines.size()) {
            selected = m_sparseSelection.erase(selected);
        } else {
            ++selected;
        }
    }
    markCommittedSelectionChanged();
}
