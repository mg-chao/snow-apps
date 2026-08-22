#include "snow_shot/presentation/screenshotselectionmodel.h"

#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <algorithm>

namespace {
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;
} // namespace

void ScreenshotSelectionModel::reset() {
    m_start = QPointF();
    m_end = QPointF();
    m_moveStart = QPointF();
    m_moveOriginalSelection = QRectF();
    m_cornerRadius = 0;
    m_shadowWidth = 0;
    m_shadowColor = QColor(0x33, 0x33, 0x33);
    m_lockedAspectRatio = 0.0;
}

QRectF ScreenshotSelectionModel::normalizedSelection() const {
    return normalizedScreenshotSelection(m_start, m_end);
}

QRect ScreenshotSelectionModel::pixelSelection() const {
    return screenshotPixelRectForSelection(normalizedSelection());
}

bool ScreenshotSelectionModel::hasPixelSelection() const {
    const QRect selection = pixelSelection();
    return selection.width() >= 1 && selection.height() >= 1;
}

void ScreenshotSelectionModel::clearSelection() {
    m_start = QPointF();
    m_end = QPointF();
}

void ScreenshotSelectionModel::setSelectionRect(const QRectF& selection) {
    const QRectF normalized = selection.normalized();
    m_start = normalized.topLeft();
    m_end = normalized.bottomRight();
}

void ScreenshotSelectionModel::setSelectionStartEnd(const QPointF& start, const QPointF& end) {
    m_start = start;
    m_end = end;
}

void ScreenshotSelectionModel::setSelectionStart(const QPointF& start) {
    m_start = start;
}

void ScreenshotSelectionModel::setSelectionEnd(const QPointF& end) {
    m_end = end;
}

void ScreenshotSelectionModel::beginMoveDrag(const QPointF& startPosition) {
    m_moveStart = startPosition;
    m_moveOriginalSelection = normalizedSelection();
}

void ScreenshotSelectionModel::rebaseMoveDrag(const QPointF& startPosition) {
    beginMoveDrag(startPosition);
}

QRectF ScreenshotSelectionModel::moveOriginalSelection() const {
    return m_moveOriginalSelection;
}

QPointF ScreenshotSelectionModel::moveStart() const {
    return m_moveStart;
}

QRectF ScreenshotSelectionModel::selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                                      const QPointF& position, const QRectF& bounds,
                                                      qreal minimumSelectionSize,
                                                      qreal lockedAspectRatioOverride) const {
    return draggedScreenshotSelectionRect(dragMode, m_moveOriginalSelection, m_moveStart, position,
                                          bounds, minimumSelectionSize,
                                          lockedAspectRatioOverride >= 0.0
                                              ? lockedAspectRatioOverride
                                              : m_lockedAspectRatio);
}

QRectF ScreenshotSelectionModel::boundedSelectionRect(const QRectF& selection, const QRectF& bounds,
                                                      bool preserveSize,
                                                      qreal minimumSelectionSize) const {
    return boundedScreenshotSelectionRect(selection, bounds, preserveSize, minimumSelectionSize);
}

bool ScreenshotSelectionModel::adjustFromToolbar(int minDx, int minDy, int maxDx, int maxDy,
                                                 const QRectF& bounds, qreal minimumSelectionSize) {
    QRectF selection = normalizedSelection();
    if (!selection.isValid() || selection.width() < minimumSelectionSize ||
        selection.height() < minimumSelectionSize) {
        return false;
    }

    selection.adjust(minDx, minDy, maxDx, maxDy);
    if (m_lockedAspectRatio > 0.0 && minDx == 0 && minDy == 0 && (maxDx != 0 || maxDy != 0)) {
        double width = std::max<qreal>(minimumSelectionSize, selection.width());
        double height = std::max<qreal>(minimumSelectionSize, selection.height());
        const int deltaValue = maxDx + maxDy;
        if (deltaValue > 0) {
            if (width * (width * m_lockedAspectRatio) > (height / m_lockedAspectRatio) * height) {
                height = width * m_lockedAspectRatio;
            } else {
                width = height / m_lockedAspectRatio;
            }
        } else {
            if (width * (width * m_lockedAspectRatio) < (height / m_lockedAspectRatio) * height) {
                height = width * m_lockedAspectRatio;
            } else {
                width = height / m_lockedAspectRatio;
            }
        }
        selection.setWidth(width);
        selection.setHeight(height);
    }

    setSelectionRect(boundedSelectionRect(selection.normalized(), bounds,
                                          minDx == maxDx && minDy == maxDy, minimumSelectionSize));
    return true;
}

int ScreenshotSelectionModel::cornerRadius() const {
    return m_cornerRadius;
}

int ScreenshotSelectionModel::shadowWidth() const {
    return m_shadowWidth;
}

QColor ScreenshotSelectionModel::shadowColor() const {
    return m_shadowColor;
}

bool ScreenshotSelectionModel::aspectRatioLocked() const {
    return m_lockedAspectRatio > 0.0;
}

bool ScreenshotSelectionModel::setCornerRadius(int radius) {
    const int clampedRadius = std::clamp(radius, 0, kScreenshotSelectionCornerRadiusMax);
    if (m_cornerRadius == clampedRadius) {
        return false;
    }

    m_cornerRadius = clampedRadius;
    return true;
}

bool ScreenshotSelectionModel::setShadowWidth(int shadowWidth) {
    const int clampedShadowWidth = std::clamp(shadowWidth, 0, kScreenshotSelectionShadowWidthMax);
    if (m_shadowWidth == clampedShadowWidth) {
        return false;
    }

    m_shadowWidth = clampedShadowWidth;
    return true;
}

void ScreenshotSelectionModel::setShadowColor(const QColor& color) {
    m_shadowColor = color.isValid() ? color : QColor(0x33, 0x33, 0x33);
}

void ScreenshotSelectionModel::toggleAspectRatioLock(qreal minimumSelectionSize) {
    if (m_lockedAspectRatio > 0.0) {
        m_lockedAspectRatio = 0.0;
        return;
    }

    const QRectF selection = normalizedSelection();
    if (selection.width() >= minimumSelectionSize && selection.height() >= minimumSelectionSize) {
        m_lockedAspectRatio = selection.height() / selection.width();
    }
}

ScreenshotSelectionParams ScreenshotSelectionModel::params(const QRect& bounds) const {
    ScreenshotSelectionParams result;
    result.selection = pixelSelection();
    result.radius = m_cornerRadius;
    result.shadowWidth = m_shadowWidth;
    result.shadowColor = m_shadowColor;
    result.lockAspectRatio = aspectRatioLocked();
    result.lockDragAspectRatio = aspectRatioLocked();
    return clampScreenshotSelectionParams(result, bounds);
}

bool ScreenshotSelectionModel::applyParams(const ScreenshotSelectionParams& params,
                                           const QRect& bounds) {
    if (bounds.isEmpty()) {
        return false;
    }

    const ScreenshotSelectionParams clamped = clampScreenshotSelectionParams(params, bounds);
    if (clamped.selection.width() < 1 || clamped.selection.height() < 1) {
        return false;
    }

    m_start = QPointF(clamped.selection.left(), clamped.selection.top());
    m_end = QPointF(clamped.selection.left() + clamped.selection.width(),
                    clamped.selection.top() + clamped.selection.height());
    m_cornerRadius = clamped.radius;
    m_shadowWidth = clamped.shadowWidth;
    setShadowColor(clamped.shadowColor);
    m_lockedAspectRatio = clamped.lockDragAspectRatio
                              ? static_cast<double>(std::max(1, clamped.selection.height())) /
                                    static_cast<double>(std::max(1, clamped.selection.width()))
                              : 0.0;
    return true;
}
