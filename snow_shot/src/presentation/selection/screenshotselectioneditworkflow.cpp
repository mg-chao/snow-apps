#include "snow_shot/presentation/screenshotselectioneditworkflow.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"

#include <utility>

ScreenshotSelectionEditWorkflow::ScreenshotSelectionEditWorkflow(
    ScreenshotSelectionEditWorkflowContext context)
    : m_context(std::move(context)) {}

void ScreenshotSelectionEditWorkflow::adjustSelectionFromToolbar(int minDx, int minDy, int maxDx,
                                                                 int maxDy) {
    if (!m_context.interaction.canResizeSelection()) {
        return;
    }

    if (!m_context.selection.adjustFromToolbar(
            minDx, minDy, maxDx, maxDy, m_context.geometry.canvasBounds(),
            snow_shot::presentation::kScreenshotSelectionMinimumSize)) {
        return;
    }
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
    if (m_context.ui.showSelectionToolbar) {
        m_context.ui.showSelectionToolbar();
    }
    if (m_context.ui.moveToolbar) {
        m_context.ui.moveToolbar();
    }
}

void ScreenshotSelectionEditWorkflow::setSelectionCornerRadiusFromToolbar(int radius) {
    if (!m_context.selection.setCornerRadius(radius)) {
        return;
    }

    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
}

void ScreenshotSelectionEditWorkflow::setSelectionShadowWidthFromToolbar(int shadowWidth) {
    if (!m_context.selection.setShadowWidth(shadowWidth)) {
        return;
    }

    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
}

void ScreenshotSelectionEditWorkflow::toggleSelectionAspectRatioLockFromToolbar() {
    m_context.selection.toggleAspectRatioLock(
        snow_shot::presentation::kScreenshotSelectionMinimumSize);
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
}

void ScreenshotSelectionEditWorkflow::openSelectionResizeModalFromToolbar() {
    const QRect bounds = selectionBounds();
    const ScreenshotSelectionParams currentParams = currentSelectionParams();
    if (bounds.isEmpty() || !m_context.selection.hasPixelSelection() ||
        !m_context.interaction.canResizeSelection()) {
        return;
    }

    hideColorPickersForScreenshotUi();
    setColorPickerSuppressedForScreenshotUi(true);
    ScreenshotSelectionResizeRequest request;
    request.currentParams = currentParams;
    request.selectionBounds = bounds;
    request.ownerWindow = ownerWindowForSelectionResizeModal();
    request.onFinished = [this]() { setColorPickerSuppressedForScreenshotUi(false); };
    if (!m_context.ui.openResizeModal ||
        !m_context.ui.openResizeModal(
            &m_context.modalParent, request,
            [this](const ScreenshotSelectionParams& params) { applySelectionParams(params); })) {
        setColorPickerSuppressedForScreenshotUi(false);
        return;
    }
}

void ScreenshotSelectionEditWorkflow::repositionToolbarForContentChange() {
    if (!m_context.interaction.canResizeSelection()) {
        return;
    }

    if (m_context.ui.repositionSelectionToolbarForContentChange) {
        m_context.ui.repositionSelectionToolbarForContentChange();
    }
}

void ScreenshotSelectionEditWorkflow::hideColorPickersForScreenshotUi() const {
    if (m_context.ui.hideColorPicker) {
        m_context.ui.hideColorPicker();
    }
}

void ScreenshotSelectionEditWorkflow::setColorPickerSuppressedForScreenshotUi(
    bool suppressed) const {
    if (m_context.ui.setColorPickerSuppressed) {
        m_context.ui.setColorPickerSuppressed(suppressed);
    }
}

void ScreenshotSelectionEditWorkflow::applySelectionParams(
    const ScreenshotSelectionParams& params) {
    const QRect bounds = selectionBounds();
    if (!m_context.selection.applyParams(params, bounds)) {
        return;
    }

    m_context.interaction.applySelectionParams();
    m_context.captureState.sessionState = ScreenshotSessionState::Editing;
    if (m_context.ui.updateOverlayState) {
        m_context.ui.updateOverlayState();
    }
    if (m_context.ui.showToolbar) {
        m_context.ui.showToolbar();
    }
}

QRect ScreenshotSelectionEditWorkflow::selectionBounds() const {
    const QRectF bounds = m_context.geometry.canvasBounds();
    if (bounds.isNull() || bounds.isEmpty()) {
        return {};
    }
    return ScreenshotHalfOpenRect::fromRectF(bounds).toAlignedQRect();
}

ScreenshotSelectionParams ScreenshotSelectionEditWorkflow::currentSelectionParams() const {
    return m_context.selection.params(selectionBounds());
}

QWidget* ScreenshotSelectionEditWorkflow::ownerWindowForSelectionResizeModal() const {
    const QRectF selection = m_context.selection.normalizedSelection();
    const CapturedDisplayModel* display =
        m_context.geometry.displayForCanvasPoint(m_context.displaySession, selection.center());
    if (display == nullptr) {
        display = m_context.geometry.displayForCanvasRect(m_context.displaySession, selection);
    }
    return m_context.displaySession.overlayForDisplay(display);
}
