#include "snow_shot/presentation/screenshotselectionmodel.h"

#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <algorithm>

namespace {
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;
constexpr qreal kNewMarqueeAspectRatio = 1.0;
} // namespace

void ScreenshotSelectionModel::reset() {
    m_cachedSelectionRegion.reset();
    m_draftVertices.clear();
    m_draftRegion.reset();
    m_region.reset();
    m_confirmedRegion = {};
    m_moveOriginalRegion = {};
    m_regionOperation = RegionOperation::Replace;
    m_start = QPointF();
    m_end = QPointF();
    m_moveStart = QPointF();
    m_moveOriginalSelection = QRectF();
    m_cornerRadius = 0;
    m_shadowWidth = 0;
    m_shadowColor = QColor(0x33, 0x33, 0x33);
    m_aspectRatioLockEnabled = false;
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
    m_cachedSelectionRegion.reset();
    m_draftVertices.clear();
    m_draftRegion.reset();
    m_region.reset();
    m_confirmedRegion = {};
    m_moveOriginalRegion = {};
    m_regionOperation = RegionOperation::Replace;
    m_start = QPointF();
    m_end = QPointF();
    m_lockedAspectRatio = 0.0;
}

void ScreenshotSelectionModel::setSelectionRect(const QRectF& selection) {
    m_cachedSelectionRegion.reset();
    m_draftVertices.clear();
    m_draftRegion.reset();
    m_region.reset();
    const QRectF normalized = selection.normalized();
    m_start = normalized.topLeft();
    m_end = normalized.bottomRight();
}

void ScreenshotSelectionModel::setSelectionStartEnd(const QPointF& start, const QPointF& end) {
    setSelectionRect(marqueeScreenshotSelectionRect(start, end));
}

void ScreenshotSelectionModel::beginMoveDrag(const QPointF& startPosition) {
    m_moveOriginalRegion = selectionRegion();
    m_moveStart = startPosition;
    m_moveOriginalSelection = normalizedSelection();
}

void ScreenshotSelectionModel::rebaseMoveDrag(const QPointF& startPosition) {
    beginMoveDrag(startPosition);
}

QRectF ScreenshotSelectionModel::moveOriginalSelection() const {
    return m_moveOriginalSelection;
}

QRectF ScreenshotSelectionModel::selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                                      const QPointF& position, const QRectF& bounds,
                                                      qreal minimumSelectionSize,
                                                      qreal lockedAspectRatioOverride) const {
    qreal lockedAspectRatio =
        lockedAspectRatioOverride >= 0.0 ? lockedAspectRatioOverride : m_lockedAspectRatio;
    if (lockedAspectRatio <= 0.0 && m_aspectRatioLockEnabled &&
        dragMode == ScreenshotSelectionDragMode::Marquee) {
        lockedAspectRatio = kNewMarqueeAspectRatio;
    }
    return draggedScreenshotSelectionRect(dragMode, m_moveOriginalSelection, m_moveStart, position,
                                          bounds, minimumSelectionSize, lockedAspectRatio);
}

QRectF ScreenshotSelectionModel::boundedSelectionRect(const QRectF& selection, const QRectF& bounds,
                                                      bool preserveSize,
                                                      qreal minimumSelectionSize) const {
    return boundedScreenshotSelectionRect(selection, bounds, preserveSize, minimumSelectionSize);
}

bool ScreenshotSelectionModel::adjustFromToolbar(int minDx, int minDy, int maxDx, int maxDy,
                                                 const QRectF& bounds, qreal minimumSelectionSize) {
    if (!rectangular() && (minDx != maxDx || minDy != maxDy)) {
        return false;
    }
    const ScreenshotRegionGeometry originalRegion = selectionRegion();
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
    if (originalRegion.rectCount() > 1) {
        setSelectionRegion(originalRegion.translated(pixelSelection().topLeft() -
                                                     originalRegion.boundingRect().topLeft()));
    }
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
    return m_aspectRatioLockEnabled;
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

bool ScreenshotSelectionModel::setAspectRatioLockEnabled(bool enabled, qreal minimumSelectionSize) {
    const bool enabledChanged = m_aspectRatioLockEnabled != enabled;
    m_aspectRatioLockEnabled = enabled;
    if (!enabled) {
        const bool ratioChanged = m_lockedAspectRatio > 0.0;
        m_lockedAspectRatio = 0.0;
        return enabledChanged || ratioChanged;
    }

    const QRectF selection = normalizedSelection();
    if (selection.width() >= minimumSelectionSize && selection.height() >= minimumSelectionSize) {
        const qreal nextAspectRatio = selection.height() / selection.width();
        const bool ratioChanged = !qFuzzyCompare(1.0 + m_lockedAspectRatio, 1.0 + nextAspectRatio);
        m_lockedAspectRatio = nextAspectRatio;
        return enabledChanged || ratioChanged;
    }
    const bool ratioChanged = m_lockedAspectRatio > 0.0;
    m_lockedAspectRatio = 0.0;
    return enabledChanged || ratioChanged;
}

void ScreenshotSelectionModel::toggleAspectRatioLock(qreal minimumSelectionSize) {
    static_cast<void>(setAspectRatioLockEnabled(!aspectRatioLocked(), minimumSelectionSize));
}

ScreenshotSelectionParams ScreenshotSelectionModel::params(const QRect& bounds) const {
    ScreenshotSelectionParams result;
    result.selection = pixelSelection();
    result.region = m_region;
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
        clearSelection();
        return false;
    }

    cancelRegionOperation();
    setSelectionRect(QRectF(clamped.selection));
    if (clamped.region) {
        setSelectionRegion(*clamped.region);
    }
    m_cornerRadius = clamped.radius;
    m_shadowWidth = clamped.shadowWidth;
    setShadowColor(clamped.shadowColor);
    m_aspectRatioLockEnabled = clamped.lockDragAspectRatio;
    m_lockedAspectRatio = clamped.lockDragAspectRatio
                              ? static_cast<double>(std::max(1, clamped.selection.height())) /
                                    static_cast<double>(std::max(1, clamped.selection.width()))
                              : 0.0;
    return true;
}

ScreenshotRegionGeometry ScreenshotSelectionModel::confirmedRegion() const {
    return regionOperationActive() ? m_confirmedRegion : selectionRegion();
}

ScreenshotRegionGeometry ScreenshotSelectionModel::selectionRegion() const {
    if (m_cachedSelectionRegion)
        return *m_cachedSelectionRegion;
    const ScreenshotRegionGeometry marquee =
        m_draftRegion.value_or(ScreenshotRegionGeometry(pixelSelection()));
    if (m_regionOperation == RegionOperation::Add)
        return *(m_cachedSelectionRegion = m_confirmedRegion.united(marquee));
    if (m_regionOperation == RegionOperation::Subtract)
        return *(m_cachedSelectionRegion = m_confirmedRegion.subtracted(marquee));
    return m_draftRegion.value_or(m_region.value_or(marquee));
}

ScreenshotRegionGeometry
ScreenshotSelectionModel::selectionRegionForMarquee(const QRectF& marquee) const {
    if (!regionOperationActive() || constructionActive())
        return selectionRegion();
    const ScreenshotRegionGeometry operand(screenshotPixelRectForSelection(marquee));
    if (operand.isEmpty())
        return m_confirmedRegion;
    return m_regionOperation == RegionOperation::Add ? m_confirmedRegion.united(operand)
                                                     : m_confirmedRegion.subtracted(operand);
}

bool ScreenshotSelectionModel::rectangular() const {
    return !constructionActive() && !regionOperationActive() && selectionRegion().rectCount() == 1;
}

bool ScreenshotSelectionModel::regionOperationActive() const {
    return m_regionOperation != RegionOperation::Replace;
}

ScreenshotSelectionModel::RegionOperation ScreenshotSelectionModel::regionOperation() const {
    return m_regionOperation;
}

QRectF ScreenshotSelectionModel::pendingMarquee() const {
    return regionOperationActive() && !constructionActive() ? normalizedSelection() : QRectF();
}

void ScreenshotSelectionModel::beginRegionOperation(RegionOperation operation) {
    const ScreenshotRegionGeometry confirmed = confirmedRegion();
    clearSelection();
    m_confirmedRegion = confirmed;
    m_regionOperation = operation;
}

void ScreenshotSelectionModel::setSelectionRegion(const ScreenshotRegionGeometry& region) {
    const ScreenshotRegionGeometry snapshot = region;
    setSelectionRect(QRectF(snapshot.boundingRect()));
    m_regionOperation = RegionOperation::Replace;
    m_confirmedRegion = {};
    if (snapshot.rectCount() > 1)
        m_region = snapshot;
}

void ScreenshotSelectionModel::commitRegionOperation() {
    if (regionOperationActive())
        setSelectionRegion(selectionRegion());
}

void ScreenshotSelectionModel::cancelRegionOperation() {
    if (regionOperationActive())
        setSelectionRegion(m_confirmedRegion);
}

void ScreenshotSelectionModel::setDraggedSelectionRect(const QRectF& rect,
                                                       ScreenshotSelectionDragMode mode) {
    if (!regionOperationActive() && mode == ScreenshotSelectionDragMode::All &&
        m_moveOriginalRegion.rectCount() > 1) {
        setSelectionRegion(m_moveOriginalRegion.translated(
            screenshotPixelRectForSelection(rect).topLeft() -
            screenshotPixelRectForSelection(m_moveOriginalSelection).topLeft()));
    } else {
        setSelectionRect(rect);
    }
}

ScreenshotResultStyle ScreenshotSelectionModel::resultStyle() const {
    ScreenshotResultStyle style{m_cornerRadius, m_shadowWidth, m_shadowColor, {}, 1.0};
    if (m_region) {
        style.region = m_region->translated(-pixelSelection().topLeft());
    }
    return style;
}

void ScreenshotSelectionModel::setDraftRegion(const ScreenshotRegionGeometry& region,
                                              const QVector<QPointF>& vertices) {
    m_cachedSelectionRegion.reset();
    m_draftVertices = vertices;
    m_draftRegion = region;
    const QRectF bounds(region.boundingRect());
    m_start = bounds.topLeft();
    m_end = bounds.bottomRight();
}

void ScreenshotSelectionModel::clearDraftRegion() {
    m_cachedSelectionRegion.reset();
    m_draftVertices.clear();
    m_draftRegion.reset();
    m_start = {};
    m_end = {};
}

void ScreenshotSelectionModel::commitDraftRegion(const QRect& canvasBounds) {
    if (m_draftRegion) {
        const auto region = selectionRegion();
        setSelectionRegion(canvasBounds.isEmpty() ? region : region.intersected(canvasBounds));
    }
}
