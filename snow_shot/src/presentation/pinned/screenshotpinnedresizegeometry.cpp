#include "screenshotpinnedresizegeometry.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

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
        const double normalizedX =
            (mousePosition.x() - reference.left()) / reference.width();
        const double normalizedY =
            (mousePosition.y() - reference.top()) / reference.height();
        topLeft = QPointF(mousePosition.x() - normalizedX * targetSize.width(),
                          mousePosition.y() - normalizedY * targetSize.height());
        break;
    }
    case ScaleAnchor::TopLeft:
        topLeft = QPointF(reference.left(), reference.top());
        break;
    case ScaleAnchor::TopRight:
        topLeft = QPointF(reference.left() + reference.width() - targetSize.width(),
                          reference.top());
        break;
    case ScaleAnchor::BottomLeft:
        topLeft = QPointF(reference.left(),
                          reference.top() + reference.height() - targetSize.height());
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
    double minimumScale, double maximumScale, QRect* result) {
    if (result == nullptr || !proposed.isValid() || proposed.isEmpty() || !reference.isValid() ||
        reference.isEmpty() || !baseline.isValid() || baseline.isEmpty() ||
        !std::isfinite(minimumScale) || !std::isfinite(maximumScale) || minimumScale <= 0.0 ||
        maximumScale < minimumScale) {
        return false;
    }

    const double scale =
        std::clamp(requestedScale(proposed.size(), baseline, handle), minimumScale, maximumScale);
    const QSize size = scaledSize(baseline, scale);
    if (!size.isValid() || size.isEmpty()) {
        return false;
    }

    QRect resized(proposed.topLeft(), size);
    attachToFixedAnchor(&resized, reference, handle);
    *result = resized;
    return true;
}
