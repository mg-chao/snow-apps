#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"

#include <algorithm>

void ScreenshotIntelligentSelectionModel::beginCaptureSession(bool smartSelectionEnabled) {
    clearTransientState();
    m_smartSelectionEnabled = smartSelectionEnabled;
    m_selectionTarget = smartSelectionEnabled
                            ? ScreenshotIntelligentSelectionTarget::WindowSubElement
                            : ScreenshotIntelligentSelectionTarget::Window;
}

void ScreenshotIntelligentSelectionModel::reset() {
    clearTransientState();
    m_smartSelectionEnabled = false;
    m_selectionTarget = ScreenshotIntelligentSelectionTarget::Window;
}

void ScreenshotIntelligentSelectionModel::clearTransientState() {
    clearHitPath();
    clearPress();
}

void ScreenshotIntelligentSelectionModel::clearHitPath() {
    m_hitRects.clear();
    m_index = -1;
}

void ScreenshotIntelligentSelectionModel::clearPress() {
    m_pressActive = false;
    m_pressPosition = QPointF();
    m_pressSelection = QRectF();
}

bool ScreenshotIntelligentSelectionModel::updateSmartSelectionEnabled(bool enabled) {
    if (m_smartSelectionEnabled == enabled) {
        return false;
    }

    m_smartSelectionEnabled = enabled;
    m_selectionTarget = enabled ? ScreenshotIntelligentSelectionTarget::WindowSubElement
                                : ScreenshotIntelligentSelectionTarget::Window;
    clearPress();
    static_cast<void>(setIndex(m_selectionTarget ==
                                       ScreenshotIntelligentSelectionTarget::Window
                                   ? static_cast<int>(m_hitRects.size() - 1)
                                   : 0));
    return true;
}

bool ScreenshotIntelligentSelectionModel::applyCanvasHitPath(const QVector<QRectF>& canvasHitRects,
                                                             const QRectF& selectableBounds,
                                                             qreal minimumSelectionSize) {
    if (canvasHitRects.isEmpty()) {
        clearHitPath();
        return false;
    }

    QVector<QRectF> boundedHitRects;
    boundedHitRects.reserve(canvasHitRects.size());
    for (const QRectF& hitRect : canvasHitRects) {
        const QRectF bounded = hitRect.intersected(selectableBounds);
        if (bounded.width() < minimumSelectionSize || bounded.height() < minimumSelectionSize) {
            continue;
        }
        if (!boundedHitRects.isEmpty() && bounded == boundedHitRects.constLast()) {
            continue;
        }
        boundedHitRects.push_back(bounded);
    }

    if (boundedHitRects.isEmpty()) {
        clearHitPath();
        return false;
    }

    if (boundedHitRects == m_hitRects) {
        return setIndex(m_index);
    }

    m_hitRects = boundedHitRects;
    return setIndex(m_selectionTarget == ScreenshotIntelligentSelectionTarget::Window
                        ? static_cast<int>(m_hitRects.size() - 1)
                        : 0);
}

bool ScreenshotIntelligentSelectionModel::setIndex(int index) {
    if (m_hitRects.isEmpty()) {
        clearHitPath();
        return false;
    }

    // Selector hit paths run from the deepest element to the outermost window.
    const int maximumIndex = static_cast<int>(m_hitRects.size() - 1);
    m_index = m_selectionTarget == ScreenshotIntelligentSelectionTarget::Window
                  ? maximumIndex
                  : std::clamp(index, 0, maximumIndex);
    return true;
}

bool ScreenshotIntelligentSelectionModel::toggleSelectionTarget() {
    if (!m_smartSelectionEnabled) {
        return false;
    }

    m_selectionTarget = m_selectionTarget == ScreenshotIntelligentSelectionTarget::Window
                            ? ScreenshotIntelligentSelectionTarget::WindowSubElement
                            : ScreenshotIntelligentSelectionTarget::Window;
    static_cast<void>(setIndex(m_selectionTarget == ScreenshotIntelligentSelectionTarget::Window
                                   ? static_cast<int>(m_hitRects.size() - 1)
                                   : 0));
    return true;
}

bool ScreenshotIntelligentSelectionModel::smartSelectionEnabled() const {
    return m_smartSelectionEnabled;
}

ScreenshotIntelligentSelectionTarget ScreenshotIntelligentSelectionModel::selectionTarget() const {
    return m_selectionTarget;
}

int ScreenshotIntelligentSelectionModel::index() const {
    return m_index;
}

bool ScreenshotIntelligentSelectionModel::hasCurrentSelection() const {
    return m_index >= 0 && m_index < m_hitRects.size();
}

QRectF ScreenshotIntelligentSelectionModel::currentSelection() const {
    return hasCurrentSelection() ? m_hitRects.at(m_index) : QRectF();
}

void ScreenshotIntelligentSelectionModel::beginPress(const QPointF& position,
                                                     const QRectF& selection) {
    m_pressActive = true;
    m_pressPosition = position;
    m_pressSelection = selection;
}

bool ScreenshotIntelligentSelectionModel::pressActive() const {
    return m_pressActive;
}

QPointF ScreenshotIntelligentSelectionModel::pressPosition() const {
    return m_pressPosition;
}

bool ScreenshotIntelligentSelectionModel::shouldStartManualDrag(const QPointF& position,
                                                                double dragStartDistance) const {
    if (!m_pressActive) {
        return false;
    }

    const QPointF delta = position - m_pressPosition;
    if (dragStartDistance <= 0.0) {
        return true;
    }

    const double distanceSquared = delta.x() * delta.x() + delta.y() * delta.y();
    return distanceSquared >= dragStartDistance * dragStartDistance;
}

QRectF ScreenshotIntelligentSelectionModel::takePressSelection() {
    const QRectF selection = m_pressSelection;
    clearPress();
    return selection;
}
