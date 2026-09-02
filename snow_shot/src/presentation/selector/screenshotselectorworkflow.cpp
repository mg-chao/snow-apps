#include "snow_shot/presentation/screenshotselectorworkflow.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <QCursor>

#include <utility>

ScreenshotSelectorWorkflow::ScreenshotSelectorWorkflow(ScreenshotSelectorWorkflowContext context)
    : m_context(std::move(context)) {}

void ScreenshotSelectorWorkflow::startRefresh() {
    if (m_context.interaction.inactive() || !m_context.displaySession.hasActiveDisplays()) {
        return;
    }

    if (!m_context.selectorService.startRefresh(excludedHwnds())) {
        return;
    }
}

void ScreenshotSelectorWorkflow::handleRefreshFinished(bool ok) {
    if (m_context.interaction.inactive()) {
        return;
    }

    if (!ok) {
        return;
    }

    if (m_context.interaction.manualSelecting() && !m_context.interaction.dragging() &&
        m_context.selection.pixelSelection().isEmpty()) {
        m_context.interaction.returnToSelectionMode(true);
        static_cast<void>(updateSelectionAt(m_context.geometry.physicalPositionForLogicalPoint(
            m_context.displaySession, QCursor::pos())));
        if (m_context.presentation.updateColorPicker) {
            m_context.presentation.updateColorPicker();
        }
    }
}

QVector<std::uintptr_t> ScreenshotSelectorWorkflow::excludedHwnds() const {
    return m_context.overlayExclusions.excludedHwnds(m_context.displaySession);
}

bool ScreenshotSelectorWorkflow::updateSelectionAt(const QPoint& physicalPoint) {
    if (!m_context.interaction.intelligentSelecting()) {
        return false;
    }

    return requestHitTest(physicalPoint);
}

bool ScreenshotSelectorWorkflow::requestHitTest(const QPoint& physicalPoint) {
    if (!m_context.interaction.intelligentSelecting() ||
        (!m_context.selectorService.ready() && !m_context.selectorService.refreshInFlight())) {
        return false;
    }

    const ScreenshotSelectorHitTestMode hitTestMode =
        m_context.intelligentSelection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::Window
            ? ScreenshotSelectorHitTestMode::Window
            : ScreenshotSelectorHitTestMode::WindowSubElement;
    return m_context.selectorService.requestHitTest(physicalPoint, hitTestMode);
}

void ScreenshotSelectorWorkflow::startNextHitTest() {
    if (m_context.interaction.intelligentSelecting()) {
        m_context.selectorService.startNextHitTest();
    }
}

void ScreenshotSelectorWorkflow::handleHitTestFinished(bool ok, const QVector<QRectF>& hitRects) {
    if (m_context.interaction.inactive()) {
        return;
    }

    if (m_context.interaction.intelligentSelecting()) {
        if (ok) {
            SNOW_SHOT_CAPTURE_PERF_SCOPE("selector.chain_apply_hit_path");
            applyHitPath(hitRects);
        }
        if (m_context.presentation.updateOverlayState) {
            SNOW_SHOT_CAPTURE_PERF_SCOPE("selector.chain_update_overlay_state");
            m_context.presentation.updateOverlayState();
        }
        if (m_context.presentation.smartSelectionResultReady) {
            SNOW_SHOT_CAPTURE_PERF_SCOPE("selector.chain_initial_resolved");
            m_context.presentation.smartSelectionResultReady(m_context.captureState.sessionId);
        }
        startNextHitTest();
    }
}

void ScreenshotSelectorWorkflow::applyHitPath(const QVector<QRectF>& hitRects) {
    QVector<QRectF> canvasHitRects;
    canvasHitRects.reserve(hitRects.size());
    for (const QRectF& hitRect : hitRects) {
        canvasHitRects.push_back(
            m_context.geometry.canvasRectForPhysicalRect(m_context.displaySession, hitRect));
    }

    if (!m_context.intelligentSelection.applyCanvasHitPath(
            canvasHitRects, m_context.geometry.canvasBounds(),
            snow_shot::presentation::kScreenshotSelectionMinimumSize)) {
        m_context.selection.clearSelection();
        return;
    }

    const QRectF currentSelection = m_context.intelligentSelection.currentSelection();
    m_context.selection.setSelectionRect(currentSelection);
}

void ScreenshotSelectorWorkflow::clearSelection() {
    m_context.intelligentSelection.clearTransientState();
    m_context.selection.clearSelection();
}

bool ScreenshotSelectorWorkflow::returnToSelection(const QPoint& physicalPoint) {
    m_context.intelligentSelection.clearPress();
    if (m_context.presentation.hideToolbar) {
        m_context.presentation.hideToolbar();
    }
    clearSelection();

    const bool selectorReady = m_context.selectorService.ready();
    m_context.interaction.returnToSelectionMode(selectorReady);
    if (!selectorReady) {
        if (m_context.presentation.updateOverlayState) {
            m_context.presentation.updateOverlayState();
        }
        startRefresh();
        return false;
    }

    if (m_context.presentation.updateOverlayCursors) {
        m_context.presentation.updateOverlayCursors();
    }
    static_cast<void>(updateSelectionAt(physicalPoint));
    return true;
}
