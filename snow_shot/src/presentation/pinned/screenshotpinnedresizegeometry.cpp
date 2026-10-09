#include "screenshotpinnedresizegeometry.h"
#include "../resizegeometry.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

std::optional<screenshot_pinned_resize_geometry::DragHandle>
screenshot_pinned_resize_geometry::hitHandle(const QPointF& position, const QRectF& outer,
                                             const QRectF& content, const QSizeF& edgeBand) {
    if (!outer.isValid() || !content.isValid() || !outer.contains(content) ||
        !std::isfinite(position.x()) || !std::isfinite(position.y()) ||
        !std::isfinite(edgeBand.width()) || !std::isfinite(edgeBand.height()) ||
        position.x() < outer.left() || position.y() < outer.top() ||
        position.x() >= outer.left() + outer.width() ||
        position.y() >= outer.top() + outer.height() || edgeBand.width() < 0 ||
        edgeBand.height() < 0)
        return {};
    const qreal rightEdge = content.left() + content.width();
    const qreal bottomEdge = content.top() + content.height();
    bool left = position.x() < content.left();
    bool right = position.x() >= rightEdge;
    bool top = position.y() < content.top();
    bool bottom = position.y() >= bottomEdge;
    if (!left && !right && !top && !bottom) {
        left = position.x() - content.left() < edgeBand.width();
        right = rightEdge - position.x() <= edgeBand.width();
        top = position.y() - content.top() < edgeBand.height();
        bottom = bottomEdge - position.y() <= edgeBand.height();
        if (left && right) {
            left = position.x() - content.left() <= rightEdge - position.x();
            right = !left;
        }
        if (top && bottom) {
            top = position.y() - content.top() <= bottomEdge - position.y();
            bottom = !top;
        }
    }
    if (left && top)
        return DragHandle::TopLeft;
    if (right && top)
        return DragHandle::TopRight;
    if (right && bottom)
        return DragHandle::BottomRight;
    if (left && bottom)
        return DragHandle::BottomLeft;
    if (left)
        return DragHandle::Left;
    if (right)
        return DragHandle::Right;
    if (top)
        return DragHandle::Top;
    if (bottom)
        return DragHandle::Bottom;
    return {};
}

QSize screenshot_pinned_resize_geometry::dpiScaledOuterSize(const QSize& pendingOuter, int oldDpi,
                                                            int newDpi, int frameMargin) {
    if (frameMargin < 0 || oldDpi <= 0 || newDpi <= 0 || pendingOuter.width() <= 2 * frameMargin ||
        pendingOuter.height() <= 2 * frameMargin)
        return {};
    const auto dimension = [oldDpi, newDpi, frameMargin](int extent) {
        const qint64 numerator = qint64(extent - 2 * frameMargin) * newDpi;
        const qint64 content = std::max<qint64>(1, (numerator + oldDpi / 2) / oldDpi);
        const qint64 outer = content + 2 * frameMargin;
        return outer <= std::numeric_limits<int>::max() ? static_cast<int>(outer) : 0;
    };
    const QSize result(dimension(pendingOuter.width()), dimension(pendingOuter.height()));
    return result.isEmpty() ? QSize() : result;
}

screenshot_pinned_resize_geometry::TrackSizeLimits
screenshot_pinned_resize_geometry::trackSizeLimits(const QSize& minimum, const QSize& maximum,
                                                   const QSize& current, const QSize& target) {
    TrackSizeLimits limits{minimum, maximum.expandedTo(minimum)};
    for (const QSize& size : {current, target}) {
        if (size.isValid() && !size.isEmpty()) {
            limits.minimum = limits.minimum.boundedTo(size);
            limits.maximum = limits.maximum.expandedTo(size);
        }
    }
    return limits;
}

namespace {
using DragHandle = screenshot_pinned_resize_geometry::DragHandle;
using ScaleAnchor = screenshot_pinned_resize_geometry::ScaleAnchor;

bool isCornerHandle(DragHandle handle) {
    switch (handle) {
    case DragHandle::TopLeft:
    case DragHandle::TopRight:
    case DragHandle::BottomRight:
    case DragHandle::BottomLeft:
        return true;
    case DragHandle::Top:
    case DragHandle::Right:
    case DragHandle::Bottom:
    case DragHandle::Left:
        return false;
    }
    return false;
}

bool isHorizontalHandle(DragHandle handle) {
    return handle == DragHandle::Left || handle == DragHandle::Right;
}

double requestedScale(const QSize& proposed, const QSize& baseline, DragHandle handle) {
    const double widthScale = static_cast<double>(proposed.width()) / baseline.width();
    const double heightScale = static_cast<double>(proposed.height()) / baseline.height();

    if (isCornerHandle(handle)) {
        return std::max(widthScale, heightScale);
    }
    return isHorizontalHandle(handle) ? widthScale : heightScale;
}

void attachToFixedAnchor(QRect* rect, const QRect& reference, DragHandle handle) {
    switch (handle) {
    case DragHandle::TopLeft:
        rect->moveBottomRight(reference.bottomRight());
        break;
    case DragHandle::Top:
    case DragHandle::TopRight:
        rect->moveBottomLeft(reference.bottomLeft());
        break;
    case DragHandle::Right:
    case DragHandle::BottomRight:
    case DragHandle::Bottom:
        rect->moveTopLeft(reference.topLeft());
        break;
    case DragHandle::BottomLeft:
    case DragHandle::Left:
        rect->moveTopRight(reference.topRight());
        break;
    }
}
} // namespace

QSize screenshot_pinned_resize_geometry::scaledSize(const QSize& baseline, double scale) {
    if (!baseline.isValid() || baseline.isEmpty() || !std::isfinite(scale) || scale <= 0.0) {
        return {};
    }

    return QSize(std::max(1, qRound(baseline.width() * scale)),
                 std::max(1, qRound(baseline.height() * scale)));
}

ScaleAnchor screenshot_pinned_resize_geometry::scaleAnchorFromSetting(QStringView value) {
    if (value == u"top_left") {
        return ScaleAnchor::TopLeft;
    }
    if (value == u"top_right") {
        return ScaleAnchor::TopRight;
    }
    if (value == u"bottom_left") {
        return ScaleAnchor::BottomLeft;
    }
    if (value == u"bottom_right") {
        return ScaleAnchor::BottomRight;
    }
    if (value == u"center") {
        return ScaleAnchor::Center;
    }
    return ScaleAnchor::MousePosition;
}

QRect screenshot_pinned_resize_geometry::anchoredScaleRect(const QRect& reference,
                                                           const QSize& targetSize,
                                                           ScaleAnchor anchor,
                                                           const QPointF& mousePosition) {
    if (!reference.isValid() || reference.isEmpty() || !targetSize.isValid() ||
        targetSize.isEmpty()) {
        return {};
    }

    QPointF topLeft;
    switch (anchor) {
    case ScaleAnchor::MousePosition: {
        const double normalizedX = (mousePosition.x() - reference.left()) / reference.width();
        const double normalizedY = (mousePosition.y() - reference.top()) / reference.height();
        topLeft = QPointF(mousePosition.x() - normalizedX * targetSize.width(),
                          mousePosition.y() - normalizedY * targetSize.height());
        break;
    }
    case ScaleAnchor::TopLeft:
        topLeft = QPointF(reference.left(), reference.top());
        break;
    case ScaleAnchor::TopRight:
        topLeft =
            QPointF(reference.left() + reference.width() - targetSize.width(), reference.top());
        break;
    case ScaleAnchor::BottomLeft:
        topLeft =
            QPointF(reference.left(), reference.top() + reference.height() - targetSize.height());
        break;
    case ScaleAnchor::BottomRight:
        topLeft = QPointF(reference.left() + reference.width() - targetSize.width(),
                          reference.top() + reference.height() - targetSize.height());
        break;
    case ScaleAnchor::Center:
        // Truncating the integer size delta keeps opposite zoom steps
        // reversible when old and new dimensions have different parity.
        topLeft = QPointF(reference.left() + (reference.width() - targetSize.width()) / 2,
                          reference.top() + (reference.height() - targetSize.height()) / 2);
        break;
    }
    return QRect(QPoint(qRound(topLeft.x()), qRound(topLeft.y())), targetSize);
}

bool screenshot_pinned_resize_geometry::proportionalResizeRect(
    const QRect& proposed, const QRect& reference, const QSize& baseline, DragHandle handle,
    double minimumScale, double maximumScale, QRect* result, int frameMargin) {
    if (result == nullptr || !proposed.isValid() || proposed.isEmpty() || !reference.isValid() ||
        reference.isEmpty() || !baseline.isValid() || baseline.isEmpty() ||
        !std::isfinite(minimumScale) || !std::isfinite(maximumScale) || minimumScale <= 0.0 ||
        maximumScale < minimumScale || frameMargin < 0 || reference.width() <= 2 * frameMargin ||
        reference.height() <= 2 * frameMargin) {
        return false;
    }

    const QSize proposedContent(std::max(1, proposed.width() - 2 * frameMargin),
                                std::max(1, proposed.height() - 2 * frameMargin));
    const double scale =
        std::clamp(requestedScale(proposedContent, baseline, handle), minimumScale, maximumScale);
    const QSize size = scaledSize(baseline, scale);
    if (!size.isValid() || size.isEmpty()) {
        return false;
    }

    QRect resized(proposed.topLeft(), size + QSize(2 * frameMargin, 2 * frameMargin));
    attachToFixedAnchor(&resized, reference, handle);
    *result = resized;
    return true;
}

bool screenshot_pinned_resize_geometry::dragResizeRect(const QRect& reference, const QPoint& delta,
                                                       const QSize& baseline, DragHandle pressed,
                                                       double minimumScale, double maximumScale,
                                                       DragHandle* effective, QRect* result,
                                                       int frameMargin) {
    if (!reference.isValid() || baseline.isEmpty() || !effective || !result ||
        !std::isfinite(minimumScale) || !std::isfinite(maximumScale) || minimumScale <= 0 ||
        maximumScale < minimumScale || frameMargin < 0 || reference.width() <= 2 * frameMargin ||
        reference.height() <= 2 * frameMargin)
        return false;
    constexpr Qt::Edges edges[] = {Qt::LeftEdge | Qt::TopEdge,     Qt::TopEdge,
                                   Qt::RightEdge | Qt::TopEdge,    Qt::RightEdge,
                                   Qt::RightEdge | Qt::BottomEdge, Qt::BottomEdge,
                                   Qt::LeftEdge | Qt::BottomEdge,  Qt::LeftEdge};
    namespace geometry = snow_shot::presentation::resize_geometry;
    const QRect content = reference.adjusted(frameMargin, frameMargin, -frameMargin, -frameMargin);
    const auto drag =
        geometry::dragGeometry(content, edges[int(pressed)], delta, edges[int(*effective)]);
    const double scale = std::clamp(requestedScale(drag.requestedSize, baseline, pressed),
                                    minimumScale, maximumScale);
    *result = geometry::anchoredRect(content, edges[int(pressed)], drag.edges,
                                     scaledSize(baseline, scale))
                  .adjusted(-frameMargin, -frameMargin, frameMargin, frameMargin);
    for (int index = 0; index < 8; ++index) {
        if (edges[index] == drag.edges) {
            *effective = static_cast<DragHandle>(index);
            break;
        }
    }
    return true;
}
