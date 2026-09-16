#include "snow_shot/presentation/screenshotselectiongeometry.h"

#include "snow_shot/presentation/screenshotgeometry.h"

#include <algorithm>
#include <cmath>

namespace {
// Resolves the half-open boundary for a selection edge dragged onto a pointer
// cell anchored against the opposite border: extending toward the pointer keeps
// the pointer's own pixel inside the selection, while a flip across the
// anchored border uses the cell origin as the new leading edge.
qreal draggedEdgeBoundary(qreal cell, qreal anchoredBoundary) {
    return cell >= anchoredBoundary ? cell + 1.0 : cell;
}

int horizontalDragDirection(ScreenshotSelectionDragMode dragMode) {
    switch (dragMode) {
    case ScreenshotSelectionDragMode::TopLeft:
    case ScreenshotSelectionDragMode::BottomLeft:
    case ScreenshotSelectionDragMode::Left:
        return -1;
    case ScreenshotSelectionDragMode::TopRight:
    case ScreenshotSelectionDragMode::BottomRight:
    case ScreenshotSelectionDragMode::Right:
        return 1;
    default:
        return 0;
    }
}

int verticalDragDirection(ScreenshotSelectionDragMode dragMode) {
    switch (dragMode) {
    case ScreenshotSelectionDragMode::TopLeft:
    case ScreenshotSelectionDragMode::TopRight:
    case ScreenshotSelectionDragMode::Top:
        return -1;
    case ScreenshotSelectionDragMode::BottomRight:
    case ScreenshotSelectionDragMode::BottomLeft:
    case ScreenshotSelectionDragMode::Bottom:
        return 1;
    default:
        return 0;
    }
}

QRectF aspectRatioLockedSelectionRect(ScreenshotSelectionDragMode dragMode, const QRectF& origin,
                                      const QPointF& delta, const QRectF& bounds,
                                      qreal minimumSelectionSize, qreal lockedAspectRatio) {
    const int horizontalDirection = horizontalDragDirection(dragMode);
    const int verticalDirection = verticalDragDirection(dragMode);
    const qreal originWidth = origin.width();
    const qreal originHeight = origin.height();
    if ((horizontalDirection == 0 && verticalDirection == 0) || originWidth <= 0.0 ||
        originHeight <= 0.0 || lockedAspectRatio <= 0.0) {
        return origin;
    }

    const qreal horizontalSpan =
        horizontalDirection == 0 ? originWidth : originWidth + horizontalDirection * delta.x();
    const qreal verticalSpan =
        verticalDirection == 0 ? originHeight : originHeight + verticalDirection * delta.y();
    const int resizedHorizontalDirection =
        horizontalSpan < 0.0 ? -horizontalDirection : horizontalDirection;
    const int resizedVerticalDirection =
        verticalSpan < 0.0 ? -verticalDirection : verticalDirection;
    const qreal horizontalScale = std::abs(horizontalSpan) / originWidth;
    const qreal verticalScale = std::abs(verticalSpan) / originHeight;
    qreal scale = 1.0;
    if (horizontalDirection != 0 && verticalDirection != 0) {
        scale = std::abs(horizontalSpan / originWidth - 1.0) >=
                        std::abs(verticalSpan / originHeight - 1.0)
                    ? horizontalScale
                    : verticalScale;
    } else {
        scale = horizontalDirection != 0 ? horizontalScale : verticalScale;
    }

    const qreal minimumScale =
        std::max(minimumSelectionSize / originWidth, minimumSelectionSize / originHeight);
    scale = std::max(scale, minimumScale);

    const auto rectForScale = [origin, horizontalDirection, verticalDirection,
                               resizedHorizontalDirection, resizedVerticalDirection, originWidth,
                               lockedAspectRatio](qreal nextScale) {
        const qreal width = originWidth * nextScale;
        const qreal height = width * lockedAspectRatio;
        qreal left = origin.center().x() - width / 2.0;
        qreal right = left + width;
        qreal top = origin.center().y() - height / 2.0;
        qreal bottom = top + height;

        if (horizontalDirection != 0) {
            const qreal anchor = horizontalDirection < 0 ? origin.right() : origin.left();
            if (resizedHorizontalDirection < 0) {
                right = anchor;
                left = right - width;
            } else {
                left = anchor;
                right = left + width;
            }
        }

        if (verticalDirection != 0) {
            const qreal anchor = verticalDirection < 0 ? origin.bottom() : origin.top();
            if (resizedVerticalDirection < 0) {
                bottom = anchor;
                top = bottom - height;
            } else {
                top = anchor;
                bottom = top + height;
            }
        }

        return QRectF(QPointF(left, top), QPointF(right, bottom));
    };

    if (bounds.isNull()) {
        return rectForScale(scale);
    }

    const QRectF normalizedBounds = bounds.normalized();
    const auto fitsBounds = [&normalizedBounds](const QRectF& rect) {
        return rect.left() >= normalizedBounds.left() && rect.top() >= normalizedBounds.top() &&
               rect.right() <= normalizedBounds.right() &&
               rect.bottom() <= normalizedBounds.bottom();
    };
    if (fitsBounds(rectForScale(scale))) {
        return rectForScale(scale);
    }

    qreal lowerScale = minimumScale;
    qreal upperScale = scale;
    for (int iteration = 0; iteration < 40; ++iteration) {
        const qreal middleScale = (lowerScale + upperScale) / 2.0;
        if (fitsBounds(rectForScale(middleScale))) {
            lowerScale = middleScale;
        } else {
            upperScale = middleScale;
        }
    }
    return rectForScale(lowerScale);
}

QRectF aspectRatioLockedMarqueeRect(const QPointF& originPosition, const QPointF& position,
                                    const QRectF& bounds, qreal lockedAspectRatio) {
    if (lockedAspectRatio <= 0.0) {
        return marqueeScreenshotSelectionRect(originPosition, position);
    }

    // Shared inclusive pointer-cell span of the drag: every axis the pointer
    // moved along already includes the cell under the pressed and current
    // pointer positions.
    const QRectF inclusive = marqueeScreenshotSelectionRect(originPosition, position);
    const QPointF originCell = screenshotPointerPixelCell(originPosition);
    const QPointF endCell = screenshotPointerPixelCell(position);
    const int horizontalDirection = endCell.x() < originCell.x() ? -1 : 1;
    const int verticalDirection = endCell.y() < originCell.y() ? -1 : 1;

    // Expand the smaller pointer span so the anchored marquee keeps its ratio.
    qreal width = std::max(inclusive.width(), inclusive.height() / lockedAspectRatio);
    qreal height = width * lockedAspectRatio;

    // Grow from the pressed cell's inclusive edges: a span extending away from
    // it starts at the cell origin, while a reverse span ends at the cell's
    // far boundary so the pressed pixel always stays inside the selection.
    qreal horizontalAnchorEdge = horizontalDirection < 0 ? inclusive.right() : inclusive.left();
    qreal verticalAnchorEdge = verticalDirection < 0 ? inclusive.bottom() : inclusive.top();

    if (!bounds.isNull()) {
        // A pointer position can round onto the exclusive canvas boundary,
        // where no real pixel cell exists; clamping the anchor edges measures
        // the ratio-preserving shrink from real canvas pixels so the selection
        // cannot grow past the canvas.
        const QRectF normalizedBounds = bounds.normalized();
        horizontalAnchorEdge =
            std::clamp(horizontalAnchorEdge, normalizedBounds.left(), normalizedBounds.right());
        verticalAnchorEdge =
            std::clamp(verticalAnchorEdge, normalizedBounds.top(), normalizedBounds.bottom());
        const qreal availableWidth = horizontalDirection < 0
                                         ? horizontalAnchorEdge - normalizedBounds.left()
                                         : normalizedBounds.right() - horizontalAnchorEdge;
        const qreal availableHeight = verticalDirection < 0
                                          ? verticalAnchorEdge - normalizedBounds.top()
                                          : normalizedBounds.bottom() - verticalAnchorEdge;
        const qreal maximumWidth =
            std::max<qreal>(0.0, std::min(availableWidth, availableHeight / lockedAspectRatio));
        width = std::min(width, maximumWidth);
        height = width * lockedAspectRatio;
    }

    const qreal left =
        horizontalDirection < 0 ? horizontalAnchorEdge - width : horizontalAnchorEdge;
    const qreal top = verticalDirection < 0 ? verticalAnchorEdge - height : verticalAnchorEdge;
    return QRectF(left, top, width, height).normalized();
}
} // namespace

QRectF normalizedScreenshotSelection(const QPointF& start, const QPointF& end) {
    return QRectF(start, end).normalized();
}

QPointF screenshotPointerPixelCell(const QPointF& position) {
    return QPointF(std::round(position.x()), std::round(position.y()));
}

QRectF marqueeScreenshotSelectionRect(const QPointF& start, const QPointF& end) {
    const QPointF originCell = screenshotPointerPixelCell(start);
    const QPointF endCell = screenshotPointerPixelCell(end);
    const QPointF topLeft(std::min(originCell.x(), endCell.x()),
                          std::min(originCell.y(), endCell.y()));
    const QSizeF span(std::abs(endCell.x() - originCell.x()) + 1.0,
                      std::abs(endCell.y() - originCell.y()) + 1.0);
    if (span.width() == 1.0 && span.height() == 1.0) {
        // A click is not a selection; keep the empty rect anchored on the cell
        // so marquee presses still reset the selection origin.
        return QRectF(topLeft, QSizeF(0.0, 0.0));
    }
    return QRectF(topLeft, span);
}

QRect screenshotPixelRectForSelection(const QRectF& selection) {
    return ScreenshotHalfOpenRect::fromRectF(selection).toAlignedQRect();
}

ScreenshotSelectionDragMode
screenshotSelectionDragModeForPoint(const QRectF& selection, const QPointF& point, bool borderOnly,
                                    qreal edgeTolerance, qreal minimumSelectionSize) {
    if (!selection.isValid() || selection.width() < minimumSelectionSize ||
        selection.height() < minimumSelectionSize) {
        return ScreenshotSelectionDragMode::None;
    }

    if (borderOnly) {
        const QRectF outer =
            selection.adjusted(-edgeTolerance, -edgeTolerance, edgeTolerance, edgeTolerance);
        if (!outer.contains(point)) {
            return ScreenshotSelectionDragMode::None;
        }

        const QRectF inner =
            selection.adjusted(edgeTolerance, edgeTolerance, -edgeTolerance, -edgeTolerance);
        if (inner.isValid() && inner.contains(point)) {
            return ScreenshotSelectionDragMode::None;
        }
    }

    int position = 0;
    if (point.y() <= selection.top() + edgeTolerance) {
        position |= 0b1000;
    }
    if (point.x() >= selection.right() - edgeTolerance) {
        position |= 0b0100;
    }
    if (point.y() >= selection.bottom() - edgeTolerance) {
        position |= 0b0010;
    }
    if (point.x() <= selection.left() + edgeTolerance) {
        position |= 0b0001;
    }

    switch (position) {
    case 0b1001:
        return ScreenshotSelectionDragMode::TopLeft;
    case 0b1100:
        return ScreenshotSelectionDragMode::TopRight;
    case 0b0110:
        return ScreenshotSelectionDragMode::BottomRight;
    case 0b0011:
        return ScreenshotSelectionDragMode::BottomLeft;
    case 0b1000:
        return ScreenshotSelectionDragMode::Top;
    case 0b0100:
        return ScreenshotSelectionDragMode::Right;
    case 0b0010:
        return ScreenshotSelectionDragMode::Bottom;
    case 0b0001:
        return ScreenshotSelectionDragMode::Left;
    default:
        return borderOnly ? ScreenshotSelectionDragMode::None : ScreenshotSelectionDragMode::All;
    }
}

QRectF boundedScreenshotSelectionRect(const QRectF& selection, const QRectF& bounds,
                                      bool preserveSize, qreal minimumSelectionSize) {
    if (bounds.isNull()) {
        return selection;
    }

    QRectF constrained = selection;
    if (preserveSize) {
        if (constrained.left() < bounds.left()) {
            constrained.translate(bounds.left() - constrained.left(), 0.0);
        }
        if (constrained.top() < bounds.top()) {
            constrained.translate(0.0, bounds.top() - constrained.top());
        }
        if (constrained.right() > bounds.right()) {
            constrained.translate(bounds.right() - constrained.right(), 0.0);
        }
        if (constrained.bottom() > bounds.bottom()) {
            constrained.translate(0.0, bounds.bottom() - constrained.bottom());
        }
        return constrained;
    }

    constrained.setLeft(std::clamp(constrained.left(), bounds.left(), bounds.right()));
    constrained.setRight(std::clamp(constrained.right(), bounds.left(), bounds.right()));
    constrained.setTop(std::clamp(constrained.top(), bounds.top(), bounds.bottom()));
    constrained.setBottom(std::clamp(constrained.bottom(), bounds.top(), bounds.bottom()));

    if (constrained.width() < minimumSelectionSize) {
        constrained.setWidth(minimumSelectionSize);
    }
    if (constrained.height() < minimumSelectionSize) {
        constrained.setHeight(minimumSelectionSize);
    }
    return constrained;
}

QRectF draggedScreenshotSelectionRect(ScreenshotSelectionDragMode dragMode, const QRectF& origin,
                                      const QPointF& originPosition, const QPointF& position,
                                      const QRectF& bounds, qreal minimumSelectionSize,
                                      qreal lockedAspectRatio) {
    // Snap the drag delta to whole pointer cells so dragged edges land on exact
    // half-open pixel boundaries instead of drifting with coordinate noise.
    const QPointF delta =
        screenshotPointerPixelCell(position) - screenshotPointerPixelCell(originPosition);
    if (lockedAspectRatio > 0.0 && dragMode == ScreenshotSelectionDragMode::Marquee) {
        // The locked marquee resolves its own inclusive cell span; route it
        // through the shared bounds clamp so every pointer-driven rect leaves
        // this function under the same canvas contract as the unlocked path.
        return boundedScreenshotSelectionRect(
            aspectRatioLockedMarqueeRect(originPosition, position, bounds, lockedAspectRatio),
            bounds, false, 0.0);
    }
    if (lockedAspectRatio > 0.0 && dragMode != ScreenshotSelectionDragMode::All &&
        dragMode != ScreenshotSelectionDragMode::None) {
        return aspectRatioLockedSelectionRect(dragMode, origin, delta, bounds, minimumSelectionSize,
                                              lockedAspectRatio);
    }

    QRectF result = origin;

    switch (dragMode) {
    case ScreenshotSelectionDragMode::Marquee:
        result = marqueeScreenshotSelectionRect(originPosition, position);
        break;
    case ScreenshotSelectionDragMode::All:
        result.translate(delta);
        break;
    case ScreenshotSelectionDragMode::TopLeft:
        result.setTopLeft(origin.topLeft() + delta);
        break;
    case ScreenshotSelectionDragMode::Top:
        result.setTop(origin.top() + delta.y());
        break;
    case ScreenshotSelectionDragMode::TopRight:
        result.setTopRight(origin.topRight() + delta);
        break;
    case ScreenshotSelectionDragMode::Right:
        result.setRight(origin.right() + delta.x());
        break;
    case ScreenshotSelectionDragMode::BottomRight:
        result.setBottomRight(origin.bottomRight() + delta);
        break;
    case ScreenshotSelectionDragMode::Bottom:
        result.setBottom(origin.bottom() + delta.y());
        break;
    case ScreenshotSelectionDragMode::BottomLeft:
        result.setBottomLeft(origin.bottomLeft() + delta);
        break;
    case ScreenshotSelectionDragMode::Left:
        result.setLeft(origin.left() + delta.x());
        break;
    case ScreenshotSelectionDragMode::None:
    default:
        break;
    }

    return boundedScreenshotSelectionRect(
        result.normalized(), bounds, dragMode == ScreenshotSelectionDragMode::All,
        dragMode == ScreenshotSelectionDragMode::Marquee ? 0.0 : minimumSelectionSize);
}

QRectF grabAdjustedScreenshotSelectionRect(ScreenshotSelectionDragMode dragMode,
                                           const QRectF& selection, const QPointF& position,
                                           const QRectF& bounds, qreal minimumSelectionSize) {
    QRectF result = selection;
    const QPointF cell = screenshotPointerPixelCell(position);

    switch (dragMode) {
    case ScreenshotSelectionDragMode::TopLeft:
        result.setTopLeft(QPointF(draggedEdgeBoundary(cell.x(), selection.right()),
                                  draggedEdgeBoundary(cell.y(), selection.bottom())));
        break;
    case ScreenshotSelectionDragMode::Top:
        result.setTop(draggedEdgeBoundary(cell.y(), selection.bottom()));
        break;
    case ScreenshotSelectionDragMode::TopRight:
        result.setTopRight(QPointF(draggedEdgeBoundary(cell.x(), selection.left()),
                                   draggedEdgeBoundary(cell.y(), selection.bottom())));
        break;
    case ScreenshotSelectionDragMode::Right:
        result.setRight(draggedEdgeBoundary(cell.x(), selection.left()));
        break;
    case ScreenshotSelectionDragMode::BottomRight:
        result.setBottomRight(QPointF(draggedEdgeBoundary(cell.x(), selection.left()),
                                      draggedEdgeBoundary(cell.y(), selection.top())));
        break;
    case ScreenshotSelectionDragMode::Bottom:
        result.setBottom(draggedEdgeBoundary(cell.y(), selection.top()));
        break;
    case ScreenshotSelectionDragMode::BottomLeft:
        result.setBottomLeft(QPointF(draggedEdgeBoundary(cell.x(), selection.right()),
                                     draggedEdgeBoundary(cell.y(), selection.top())));
        break;
    case ScreenshotSelectionDragMode::Left:
        result.setLeft(draggedEdgeBoundary(cell.x(), selection.right()));
        break;
    case ScreenshotSelectionDragMode::Marquee:
    case ScreenshotSelectionDragMode::All:
    case ScreenshotSelectionDragMode::None:
    default:
        return selection;
    }

    return boundedScreenshotSelectionRect(result.normalized(), bounds, false, minimumSelectionSize);
}

std::optional<QPointF> screenshotSelectionDragAnchor(const QRectF& selection,
                                                     ScreenshotSelectionDragMode dragMode,
                                                     const QPointF& position,
                                                     qreal minimumSelectionSize) {
    if (dragMode == ScreenshotSelectionDragMode::Marquee) {
        return std::nullopt;
    }
    if (!selection.isValid() || selection.width() < minimumSelectionSize ||
        selection.height() < minimumSelectionSize) {
        return std::nullopt;
    }

    switch (dragMode) {
    case ScreenshotSelectionDragMode::Marquee:
        return std::nullopt;
    case ScreenshotSelectionDragMode::All:
        return QPointF(std::clamp(position.x(), selection.left(), selection.right()),
                       std::clamp(position.y(), selection.top(), selection.bottom()));
    case ScreenshotSelectionDragMode::TopLeft:
        return selection.topLeft();
    case ScreenshotSelectionDragMode::Top:
        return QPointF(std::clamp(position.x(), selection.left(), selection.right()),
                       selection.top());
    case ScreenshotSelectionDragMode::TopRight:
        return selection.topRight();
    case ScreenshotSelectionDragMode::Right:
        return QPointF(selection.right(),
                       std::clamp(position.y(), selection.top(), selection.bottom()));
    case ScreenshotSelectionDragMode::BottomRight:
        return selection.bottomRight();
    case ScreenshotSelectionDragMode::Bottom:
        return QPointF(std::clamp(position.x(), selection.left(), selection.right()),
                       selection.bottom());
    case ScreenshotSelectionDragMode::BottomLeft:
        return selection.bottomLeft();
    case ScreenshotSelectionDragMode::Left:
        return QPointF(selection.left(),
                       std::clamp(position.y(), selection.top(), selection.bottom()));
    case ScreenshotSelectionDragMode::None:
    default:
        return std::nullopt;
    }
}
