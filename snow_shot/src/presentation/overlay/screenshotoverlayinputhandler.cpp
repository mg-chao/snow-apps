#include "snow_shot/presentation/screenshotoverlayinputhandler.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "../services/screenshotlifecycleperfinstrumentation.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QCursor>

#include <algorithm>
#include <utility>

namespace {
constexpr qreal kSelectionEdgeTolerance = 8.0;
constexpr qreal kEqualWidthHeightAspectRatio = 1.0;

bool wheelAdjustsStrokeWidth(ScreenshotActiveTool tool) {
    switch (tool) {
    case ScreenshotActiveTool::Shape:
    case ScreenshotActiveTool::Arrow:
    case ScreenshotActiveTool::Line:
    case ScreenshotActiveTool::FreeDraw:
    case ScreenshotActiveTool::RectangleHighlight:
    case ScreenshotActiveTool::PenHighlight:
        return true;
    default:
        return false;
    }
}

bool recognitionTool(ScreenshotActiveTool tool) {
    return tool == ScreenshotActiveTool::Ocr || tool == ScreenshotActiveTool::Table ||
           tool == ScreenshotActiveTool::Qr;
}

bool screenshotCompletionGestureTool(ScreenshotActiveTool tool) {
    return tool != ScreenshotActiveTool::Select && tool != ScreenshotActiveTool::Ocr &&
           tool != ScreenshotActiveTool::Table && tool != ScreenshotActiveTool::Qr;
}

} // namespace

ScreenshotOverlayInputHandler::ScreenshotOverlayInputHandler(
    ScreenshotOverlayInputHandlerContext context)
    : m_context(std::move(context)) {}

ScreenshotSelectionDragMode ScreenshotOverlayInputHandler::selectionResizeDragModeAtCanvasPosition(
    const QPointF& canvasPosition) const {
    if (!(m_context.interaction.editing() || m_context.interaction.scrollingCapture() ||
          m_context.interaction.movingSelection())) {
        return ScreenshotSelectionDragMode::None;
    }
    return dragModeForVirtualPosition(canvasPosition, true);
}

bool ScreenshotOverlayInputHandler::beginSelectionResizeAtCanvasPosition(
    const QPointF& canvasPosition) {
    const ScreenshotSelectionDragMode dragMode =
        selectionResizeDragModeAtCanvasPosition(canvasPosition);
    if (dragMode == ScreenshotSelectionDragMode::None || m_context.interaction.dragging()) {
        return false;
    }

    const ScreenshotActiveTool activeTool = m_context.interaction.activeTool();
    if (m_context.interaction.scrollingCapture()) {
        m_scrollingCaptureSelectionResize = true;
        m_context.actions.pauseScrollingCapture();
    } else if (m_context.interaction.editing() && activeTool != ScreenshotActiveTool::Move) {
        m_toolBeforeSelectionResize = activeTool;
    } else if (!m_context.interaction.movingSelection()) {
        return false;
    }

    const CapturedDisplayModel* display =
        m_context.geometry.displayForCanvasPoint(m_context.displaySession, canvasPosition);
    ScreenshotOverlayWindow* overlay =
        display != nullptr ? m_context.displaySession.overlayForDisplay(display) : nullptr;
    if (m_toolBeforeSelectionResize.has_value()) {
        if (!m_context.actions.activateToolForSelectionResize(ScreenshotActiveTool::Move)) {
            m_toolBeforeSelectionResize.reset();
            restoreScrollingCaptureAfterFailedResize();
            return false;
        }
    } else {
        m_context.interaction.setMoveTool(m_context.selection.hasPixelSelection(), false);
    }
    beginSelectionDrag(overlay, canvasPosition, dragMode);
    if (!m_context.interaction.dragging()) {
        restoreToolAfterSelectionResize();
        restoreScrollingCaptureAfterFailedResize();
    }
    return m_context.interaction.dragging();
}

void ScreenshotOverlayInputHandler::updateSelectionResizeAtCanvasPosition(
    const QPointF& canvasPosition) {
    if (m_context.interaction.dragging()) {
        updateSelectionDrag(canvasPosition);
    }
}

void ScreenshotOverlayInputHandler::finishSelectionResizeAtCanvasPosition(
    const QPointF& canvasPosition) {
    if (!m_context.interaction.dragging()) {
        return;
    }
    const CapturedDisplayModel* display =
        m_context.geometry.displayForCanvasPoint(m_context.displaySession, canvasPosition);
    ScreenshotOverlayWindow* overlay =
        display != nullptr ? m_context.displaySession.overlayForDisplay(display) : nullptr;
    finishSelectionDrag(overlay, {}, canvasPosition);
}

void ScreenshotOverlayInputHandler::handleMousePress(ScreenshotOverlayWindow* overlay,
                                                     const QPointF& localPosition) {
    if (m_canvasColorSamplingArmed) {
        m_canvasColorSamplingArmed = false;
        static_cast<void>(m_context.actions.sampleCanvasColor(overlay, localPosition));
        return;
    }
    updateGuideLines(overlay, localPosition);
    m_context.actions.updateColorPickerForOverlay(overlay, localPosition);
    const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
    const ScreenshotActiveTool activeTool = m_context.interaction.activeTool();
    const ScreenshotSelectionDragMode borderDragMode =
        dragModeForVirtualPosition(virtualPosition, true);
    if ((m_context.interaction.scrollingCapture() ||
         (m_context.interaction.editing() && activeTool != ScreenshotActiveTool::Move)) &&
        borderDragMode != ScreenshotSelectionDragMode::None) {
        if (m_context.interaction.scrollingCapture()) {
            m_scrollingCaptureSelectionResize = true;
            m_context.actions.pauseScrollingCapture();
        } else {
            m_toolBeforeSelectionResize = activeTool;
        }
        if (m_toolBeforeSelectionResize.has_value()) {
            if (!m_context.actions.activateToolForSelectionResize(ScreenshotActiveTool::Move)) {
                m_toolBeforeSelectionResize.reset();
                restoreScrollingCaptureAfterFailedResize();
                return;
            }
        } else {
            m_context.interaction.setMoveTool(m_context.selection.hasPixelSelection(), false);
        }
        beginSelectionDrag(overlay, virtualPosition, borderDragMode);
        if (!m_context.interaction.dragging()) {
            restoreToolAfterSelectionResize();
            restoreScrollingCaptureAfterFailedResize();
        }
        return;
    }
    if (m_context.interaction.movingSelection()) {
        const ScreenshotSelectionDragMode hitMode =
            dragModeForVirtualPosition(virtualPosition, false);
        if (hitMode != ScreenshotSelectionDragMode::None) {
            beginSelectionDrag(overlay, virtualPosition, hitMode);
        }
        return;
    }

    if (m_context.interaction.intelligentSelecting()) {
        handleIntelligentSelectionPress(virtualPosition);
        return;
    }

    if (!m_context.interaction.manualSelecting()) {
        return;
    }

    // Manual mode can retain a live selection (for example while the selector
    // is unavailable or after a selection is restored). In that state, a press
    // inside the rectangle must move the existing selection instead of
    // restarting the marquee from the press point.
    const ScreenshotSelectionDragMode hitMode =
        dragModeForVirtualPosition(virtualPosition, false);
    beginSelectionDrag(overlay, virtualPosition,
                       hitMode == ScreenshotSelectionDragMode::None
                           ? ScreenshotSelectionDragMode::Marquee
                           : hitMode);
}

void ScreenshotOverlayInputHandler::beginSelectionDrag(ScreenshotOverlayWindow* overlay,
                                                       const QPointF& virtualPosition,
                                                       ScreenshotSelectionDragMode dragMode) {
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("interaction.selection_drag_begin"));
    if (dragMode == ScreenshotSelectionDragMode::None) {
        return;
    }

    const ScreenshotSelectionDragMode requestedDragMode = dragMode;
    if (m_moveEntireSelectionShortcut && dragMode != ScreenshotSelectionDragMode::Marquee) {
        dragMode = ScreenshotSelectionDragMode::All;
        m_moveDragModeBeforeShortcut = requestedDragMode == ScreenshotSelectionDragMode::All
                                           ? ScreenshotSelectionDragMode::None
                                           : requestedDragMode;
    } else {
        m_moveDragModeBeforeShortcut = ScreenshotSelectionDragMode::None;
    }
    if (!m_context.interaction.enterSelectionDrag(dragMode)) {
        return;
    }
    if (m_keepSelectionAspectRatioShortcut) {
        m_aspectShortcutUsedForSelectionDrag = true;
    }
    m_context.captureState.sessionState = ScreenshotSessionState::OverlayVisible;
    m_context.actions.pauseIntelligentSelection();
    if (dragMode == ScreenshotSelectionDragMode::Marquee) {
        m_context.selection.setSelectionStartEnd(virtualPosition, virtualPosition);
        m_context.intelligentSelection.clearHitPath();
        m_marqueeAnchor = virtualPosition;
    } else {
        m_marqueeAnchor = QPointF();
    }
    m_lastMoveDragPosition = virtualPosition;
    m_context.selection.beginMoveDrag(virtualPosition);
    m_context.actions.hideMainToolbar();
    m_context.actions.updateOverlayState();
    m_context.actions.setOverlayCursor(overlay, dragMode);
    m_context.actions.updateColorPickerForSelectionDrag(virtualPosition);
}

void ScreenshotOverlayInputHandler::handleIntelligentSelectionPress(
    const QPointF& virtualPosition) {
    m_context.intelligentSelection.beginPress(virtualPosition,
                                              m_context.intelligentSelection.currentSelection());
}

bool ScreenshotOverlayInputHandler::shouldHandleMouseEvent(
    const ScreenshotOverlayWindow* overlay, const QPointF& localPosition, bool) const {
    if (m_canvasColorSamplingArmed) {
        return true;
    }
    const ScreenshotActiveTool activeTool = m_context.interaction.activeTool();
    if ((m_context.interaction.scrollingCapture() ||
         (m_context.interaction.editing() && activeTool != ScreenshotActiveTool::Move)) &&
        dragModeForPosition(overlay, localPosition, true) != ScreenshotSelectionDragMode::None) {
        return true;
    }
    if (recognitionTool(activeTool)) {
        return true;
    }
    if (m_context.interaction.selecting()) {
        return true;
    }
    if (m_context.interaction.dragging()) {
        return true;
    }
    if (m_context.interaction.movingSelection()) {
        return dragModeForPosition(overlay, localPosition, false) !=
               ScreenshotSelectionDragMode::None;
    }
    return false;
}

void ScreenshotOverlayInputHandler::handleMouseMove(ScreenshotOverlayWindow* overlay,
                                                    const QPointF& localPosition) {
    if (m_canvasColorSamplingArmed) {
        m_context.actions.previewCanvasColor(overlay, localPosition);
        return;
    }
    updateGuideLines(overlay, localPosition);
    if (m_context.interaction.intelligentSelecting()) {
        const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
        handleIntelligentSelectionMove(overlay, localPosition, virtualPosition);
        return;
    }

    if (!m_context.interaction.dragging()) {
        handleHoverMove(overlay, localPosition);
        return;
    }

    const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
    updateSelectionDrag(virtualPosition);
}

void ScreenshotOverlayInputHandler::handleIntelligentSelectionMove(ScreenshotOverlayWindow* overlay,
                                                                   const QPointF& localPosition,
                                                                   const QPointF& virtualPosition) {
    if (m_context.intelligentSelection.pressActive()) {
        if (m_context.intelligentSelection.shouldStartManualDrag(
                virtualPosition, QApplication::startDragDistance())) {
            const QPointF pressPosition = m_context.intelligentSelection.pressPosition();
            m_context.intelligentSelection.clearPress();
            beginSelectionDrag(overlay, pressPosition, ScreenshotSelectionDragMode::Marquee);
            updateSelectionDrag(virtualPosition);
        }
        return;
    }

    requestIntelligentSelectionHitTest(virtualPosition);
    m_context.actions.updateColorPickerForOverlay(overlay, localPosition);
}

void ScreenshotOverlayInputHandler::handleHoverMove(ScreenshotOverlayWindow* overlay,
                                                    const QPointF& localPosition) {
    const ScreenshotActiveTool activeTool = m_context.interaction.activeTool();
    if (m_context.interaction.movingSelection() ||
        (m_context.interaction.manualSelecting() && m_context.selection.hasPixelSelection()) ||
        m_context.interaction.scrollingCapture() ||
        (m_context.interaction.editing() && activeTool != ScreenshotActiveTool::Move)) {
        const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
        const bool borderOnly = m_context.interaction.editing() ||
                                m_context.interaction.scrollingCapture();
        ScreenshotSelectionDragMode dragMode =
            dragModeForVirtualPosition(virtualPosition, borderOnly);
        if (m_context.interaction.manualSelecting() &&
            dragMode == ScreenshotSelectionDragMode::None) {
            dragMode = ScreenshotSelectionDragMode::Marquee;
        }
        m_context.actions.setOverlayCursor(overlay, dragMode);
    }
    m_context.actions.updateColorPickerForOverlay(overlay, localPosition);
}

void ScreenshotOverlayInputHandler::updateSelectionDrag(const QPointF& virtualPosition) {
    const QRectF previousSelection = m_context.selection.normalizedSelection();
    m_lastMoveDragPosition = virtualPosition;
    if (m_keepSelectionAspectRatioShortcut && m_context.interaction.dragging()) {
        m_aspectShortcutUsedForSelectionDrag = true;
    }
    const QRectF dragged = selectionRectForDrag(m_context.interaction.dragMode(), virtualPosition);
    if (m_context.interaction.dragMode() == ScreenshotSelectionDragMode::All &&
        m_moveDragModeBeforeShortcut == ScreenshotSelectionDragMode::Marquee) {
        // Follow the actual bounded translation, not the raw pointer delta.
        m_marqueeAnchor += dragged.topLeft() - previousSelection.topLeft();
    }
    m_context.selection.setSelectionRect(dragged);
    if (m_moveEntireSelectionShortcut &&
        m_context.interaction.dragMode() == ScreenshotSelectionDragMode::Marquee &&
        m_context.selection.hasPixelSelection()) {
        // Space may be pressed immediately after mouse-down, before the
        // marquee has a non-zero size. Defer the mode switch until the first
        // usable rectangle exists so movement still translates that rectangle.
        m_moveDragModeBeforeShortcut = ScreenshotSelectionDragMode::Marquee;
        m_context.selection.rebaseMoveDrag(virtualPosition);
        static_cast<void>(m_context.interaction.enterSelectionDrag(
            ScreenshotSelectionDragMode::All));
    }
    m_context.actions.updateOverlayState();
    m_context.actions.updateColorPickerForSelectionDrag(virtualPosition);
}

void ScreenshotOverlayInputHandler::handleMouseRelease(ScreenshotOverlayWindow* overlay,
                                                       const QPointF& localPosition) {
    const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
    if (m_context.interaction.intelligentSelecting()) {
        handleIntelligentSelectionRelease(virtualPosition);
        return;
    }

    if (!m_context.interaction.dragging()) {
        return;
    }
    finishSelectionDrag(overlay, localPosition, virtualPosition);
}

void ScreenshotOverlayInputHandler::handleIntelligentSelectionRelease(
    const QPointF& virtualPosition) {
    if (!m_context.intelligentSelection.pressActive()) {
        return;
    }

    const QRectF pressSelection = m_context.intelligentSelection.takePressSelection();
    if (pressSelection.isValid() && !pressSelection.isEmpty() &&
        pressSelection.contains(virtualPosition)) {
        m_context.selection.setSelectionRect(pressSelection);
        confirmSelection();
        return;
    }

    requestIntelligentSelectionHitTest(virtualPosition);
}

void ScreenshotOverlayInputHandler::finishSelectionDrag(ScreenshotOverlayWindow* overlay,
                                                        const QPointF& localPosition,
                                                        const QPointF& virtualPosition) {
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("interaction.selection_drag_finish"));
    if (m_keepSelectionAspectRatioShortcut && m_context.interaction.dragging()) {
        m_aspectShortcutUsedForSelectionDrag = true;
    }
    m_lastMoveDragPosition = virtualPosition;
    const QRectF dragged = selectionRectForDrag(m_context.interaction.dragMode(), virtualPosition);
    m_context.selection.setSelectionRect(dragged);
    m_context.interaction.finishDrag();
    finishTransientDrag();
    const bool restoringCanvasTool = m_toolBeforeSelectionResize.has_value();
    const bool resumingScrollingCapture = m_scrollingCaptureSelectionResize;
    m_scrollingCaptureSelectionResize = false;
    confirmSelection();
    restoreToolAfterSelectionResize();
    if (m_context.interaction.manualSelecting() || restoringCanvasTool ||
        resumingScrollingCapture) {
        m_context.actions.updateOverlayState();
    }
    if (resumingScrollingCapture) {
        m_context.actions.resumeScrollingCapture();
    } else if (!m_context.interaction.manualSelecting()) {
        m_context.actions.showSelectionToolbar();
        m_context.actions.setOverlayCursor(
            overlay, dragModeForVirtualPosition(virtualPosition,
                                                m_context.interaction.editing()));
    }
    m_context.actions.updateColorPickerForOverlay(overlay, localPosition);
}

bool ScreenshotOverlayInputHandler::handleRightClick(ScreenshotOverlayWindow* overlay,
                                                     const QPointF& localPosition) {
    if (m_canvasColorSamplingArmed) {
        cancelCanvasColorSampling();
        return true;
    }
    if (recognitionTool(m_context.interaction.activeTool())) {
        return true;
    }
    if (!m_context.interaction.moveToolActive()) {
        return false;
    }

    const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
    const QPoint physicalPoint = physicalPositionForCanvasPoint(virtualPosition);
    if (m_context.interaction.intelligentSelecting()) {
        resetTransientShortcuts();
        m_context.actions.cancelCapture();
        return true;
    }

    if (m_context.interaction.manualSelecting() || m_context.interaction.movingSelection()) {
        resetTransientShortcuts();
        if (m_context.actions.returnToCurrentScreenshot()) {
            return true;
        }
        m_context.actions.returnToIntelligentSelection(physicalPoint);
        return true;
    }

    return false;
}

bool ScreenshotOverlayInputHandler::handleWheel(ScreenshotOverlayWindow* overlay,
                                                const QPointF& localPosition,
                                                const QPoint& angleDelta,
                                                const QPoint& pixelDelta) {
    if (m_context.interaction.scrollingCapture()) {
        return false;
    }
    if (recognitionTool(m_context.interaction.activeTool())) {
        return true;
    }
    const int deltaY = !pixelDelta.isNull() ? pixelDelta.y() : angleDelta.y();
    if (deltaY != 0 && wheelAdjustsStrokeWidth(m_context.interaction.activeTool())) {
        return m_context.actions.stepStrokeWidth(deltaY > 0 ? 1 : -1);
    }
    if (m_context.interaction.activeTool() == ScreenshotActiveTool::Select && deltaY != 0) {
        return m_context.actions.stepSelectionOpacity(deltaY > 0 ? 1 : -1);
    }
    if (m_context.interaction.activeTool() == ScreenshotActiveTool::Spotlight && deltaY != 0) {
        return m_context.actions.stepSpotlightOpacity(deltaY > 0 ? 1 : -1);
    }
    if (m_context.interaction.activeTool() == ScreenshotActiveTool::RectangleFilter &&
        deltaY != 0) {
        return m_context.actions.stepFilterIntensity(deltaY > 0 ? 1 : -1);
    }
    if (m_context.interaction.activeTool() == ScreenshotActiveTool::PenFilter && deltaY != 0) {
        return m_context.actions.stepPenFilterStrokeWidth(deltaY > 0 ? 1 : -1);
    }
    if (m_context.interaction.activeTool() == ScreenshotActiveTool::Watermark && deltaY != 0) {
        return m_context.actions.stepWatermarkFontSize(deltaY > 0 ? 1 : -1);
    }

    if (!m_context.interaction.intelligentSelecting()) {
        return false;
    }

    const QPointF virtualPosition = virtualPositionForOverlay(overlay, localPosition);
    requestIntelligentSelectionHitTest(virtualPosition);

    if (deltaY > 0) {
        setIntelligentSelectionIndex(m_context.intelligentSelection.index() + 1);
    } else if (deltaY < 0) {
        setIntelligentSelectionIndex(m_context.intelligentSelection.index() - 1);
    }

    m_context.actions.updateOverlayState();
    return true;
}

bool ScreenshotOverlayInputHandler::shouldBlockUnhandledKeyInput() const {
    return recognitionTool(m_context.interaction.activeTool());
}

bool ScreenshotOverlayInputHandler::activateMoveEntireSelectionShortcut() {
    if (!(m_context.interaction.movingSelection() ||
          m_context.interaction.modifyingSelection() ||
          m_context.interaction.manualSelecting()) ||
        !m_context.actions.localShortcutInputAllowed()) {
        return false;
    }

    if (m_context.interaction.manualSelecting() &&
        !m_context.selection.hasPixelSelection() &&
        !(m_context.interaction.dragging() &&
          m_context.interaction.dragMode() == ScreenshotSelectionDragMode::Marquee)) {
        return false;
    }

    if (!m_moveEntireSelectionShortcut) {
        m_moveEntireSelectionShortcut = true;
    }
    if (m_context.interaction.dragging() &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::All &&
        (m_context.selection.hasPixelSelection() ||
         m_context.interaction.dragMode() != ScreenshotSelectionDragMode::Marquee)) {
        m_moveDragModeBeforeShortcut = m_context.interaction.dragMode();
        // Rebase immediately so the next pointer delta translates the
        // current resized rectangle instead of being discarded.
        m_context.selection.rebaseMoveDrag(m_lastMoveDragPosition);
        static_cast<void>(m_context.interaction.enterSelectionDrag(
            ScreenshotSelectionDragMode::All));
    }
    return true;
}

bool ScreenshotOverlayInputHandler::activateKeepSelectionAspectRatioShortcut(
    bool cycleColorFormatIfUnused) {
    if (!(m_context.interaction.movingSelection() || m_context.interaction.modifyingSelection() ||
          m_context.interaction.manualSelecting() || m_context.interaction.editing()) ||
        recognitionTool(m_context.interaction.activeTool()) ||
        !m_context.actions.localShortcutInputAllowed()) {
        return false;
    }

    if (m_keepSelectionAspectRatioShortcut) {
        return true;
    }
    m_aspectShortcutUsedForSelectionDrag = m_context.interaction.dragging();
    m_cycleColorFormatIfAspectShortcutUnused = cycleColorFormatIfUnused;
    if (m_context.interaction.dragging() &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::All &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::Marquee &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::None) {
        m_context.selection.rebaseMoveDrag(m_lastMoveDragPosition);
    }
    m_keepSelectionAspectRatioShortcut = true;
    return true;
}

bool ScreenshotOverlayInputHandler::releaseMoveEntireSelectionShortcut() {
    if (!m_moveEntireSelectionShortcut) {
        return false;
    }
    if (m_context.interaction.dragging() &&
        m_moveDragModeBeforeShortcut == ScreenshotSelectionDragMode::Marquee) {
        // Resume the original marquee from its translated fixed corner. The
        // visible rectangle remains unchanged at the modifier transition.
        static_cast<void>(m_context.interaction.enterSelectionDrag(
            ScreenshotSelectionDragMode::Marquee));
        m_context.selection.beginMoveDrag(m_marqueeAnchor);
    } else if (m_context.interaction.dragging() &&
               m_moveDragModeBeforeShortcut != ScreenshotSelectionDragMode::None) {
        // Space is a temporary movement modifier. Resume the original edge
        // resize mode from the current pointer position so releasing it does
        // not leave the gesture translating the whole selection.
        static_cast<void>(m_context.interaction.enterSelectionDrag(
            m_moveDragModeBeforeShortcut));
        m_context.selection.rebaseMoveDrag(m_lastMoveDragPosition);
    }
    m_moveEntireSelectionShortcut = false;
    m_moveDragModeBeforeShortcut = ScreenshotSelectionDragMode::None;
    return true;
}

bool ScreenshotOverlayInputHandler::releaseKeepSelectionAspectRatioShortcut() {
    if (!m_keepSelectionAspectRatioShortcut) {
        return false;
    }
    if (m_context.interaction.dragging()) {
        m_aspectShortcutUsedForSelectionDrag = true;
    }
    if (m_context.interaction.dragging() &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::All &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::Marquee &&
        m_context.interaction.dragMode() != ScreenshotSelectionDragMode::None) {
        m_aspectShortcutUsedForSelectionDrag = true;
        m_context.selection.rebaseMoveDrag(m_lastMoveDragPosition);
    }
    const bool cycleColorFormat =
        m_cycleColorFormatIfAspectShortcutUnused && !m_aspectShortcutUsedForSelectionDrag;
    m_keepSelectionAspectRatioShortcut = false;
    m_aspectShortcutUsedForSelectionDrag = false;
    m_cycleColorFormatIfAspectShortcutUnused = false;
    if (cycleColorFormat) {
        static_cast<void>(m_context.actions.cycleColorPickerFormat());
    }
    return true;
}

bool ScreenshotOverlayInputHandler::toggleIntelligentSelectionTargetShortcut() {
    if (!m_context.interaction.intelligentSelecting() ||
        !m_context.intelligentSelection.toggleSelectionTarget()) {
        return false;
    }

    m_context.intelligentSelection.clearPress();
    if (m_context.intelligentSelection.hasCurrentSelection()) {
        m_context.selection.setSelectionRect(m_context.intelligentSelection.currentSelection());
    } else {
        m_context.selection.clearSelection();
    }
    m_context.actions.requestUiSelectorHitTest(
        m_context.geometry.physicalPositionForLogicalPoint(m_context.displaySession,
                                                           QCursor::pos()));
    m_context.actions.updateOverlayState();
    return true;
}

void ScreenshotOverlayInputHandler::requestIntelligentSelectionHitTest(
    const QPointF& virtualPosition) {
    m_context.actions.requestUiSelectorHitTest(physicalPositionForCanvasPoint(virtualPosition));
}

void ScreenshotOverlayInputHandler::setIntelligentSelectionIndex(int index) {
    if (!m_context.intelligentSelection.setIndex(index)) {
        m_context.selection.clearSelection();
        return;
    }

    m_context.selection.setSelectionRect(m_context.intelligentSelection.currentSelection());
}

void ScreenshotOverlayInputHandler::confirmSelection() {
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("interaction.selection_confirm_begin"));
    if (m_context.interaction.dragging()) {
        return;
    }
    const QRect selection = m_context.selection.pixelSelection();
    if (selection.width() < 1 || selection.height() < 1) {
        return;
    }

    m_context.interaction.confirmSelection();
    m_context.captureState.sessionState = ScreenshotSessionState::Editing;
    m_context.intelligentSelection.clearPress();
    m_context.actions.updateOverlayState();
    m_context.actions.showToolbar();
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("interaction.selection_toolbar_requested"));
    m_context.actions.selectionConfirmed();
}

void ScreenshotOverlayInputHandler::handleUnhandledLeftDoubleClick() {
    if (!(m_context.interaction.movingSelection() || m_context.interaction.editing()) ||
        !m_context.selection.hasPixelSelection() ||
        !screenshotCompletionGestureTool(m_context.interaction.activeTool())) {
        return;
    }
    m_context.actions.executeConfiguredCompletionAction(
        snow_shot::storage::ScreenshotSettings().doubleClickAction());
}

void ScreenshotOverlayInputHandler::handleUnhandledMiddleClick() {
    if (!(m_context.interaction.movingSelection() || m_context.interaction.editing()) ||
        !m_context.selection.hasPixelSelection() ||
        !screenshotCompletionGestureTool(m_context.interaction.activeTool())) {
        return;
    }
    m_context.actions.executeConfiguredCompletionAction(
        snow_shot::storage::ScreenshotSettings().middleMouseButtonAction());
}

void ScreenshotOverlayInputHandler::updateGuideLines(ScreenshotOverlayWindow* overlay,
                                                     const QPointF& localPosition) const {
    m_context.actions.updateGuideLinesForOverlay(overlay, localPosition);
}

QPointF
ScreenshotOverlayInputHandler::virtualPositionForOverlay(const ScreenshotOverlayWindow* overlay,
                                                         const QPointF& localPosition) const {
    return m_context.geometry.canvasPositionForOverlayLocalPoint(m_context.displaySession, overlay,
                                                                 localPosition);
}

QPoint ScreenshotOverlayInputHandler::physicalPositionForCanvasPoint(const QPointF& point) const {
    return m_context.geometry.physicalPositionForCanvasPoint(m_context.displaySession, point);
}

ScreenshotSelectionDragMode
ScreenshotOverlayInputHandler::dragModeForVirtualPosition(const QPointF& virtualPosition,
                                                          bool borderOnly) const {
    return screenshotSelectionDragModeForPoint(
        m_context.selection.normalizedSelection(), virtualPosition, borderOnly,
        kSelectionEdgeTolerance, snow_shot::presentation::kScreenshotSelectionMinimumSize);
}

ScreenshotSelectionDragMode ScreenshotOverlayInputHandler::dragModeForPosition(
    const ScreenshotOverlayWindow* overlay, const QPointF& localPosition, bool borderOnly) const {
    return dragModeForVirtualPosition(virtualPositionForOverlay(overlay, localPosition),
                                      borderOnly);
}

QRectF ScreenshotOverlayInputHandler::selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                                           const QPointF& position) const {
    if (m_keepSelectionAspectRatioShortcut && dragMode != ScreenshotSelectionDragMode::All &&
        dragMode != ScreenshotSelectionDragMode::None) {
        const QRectF origin = m_context.selection.moveOriginalSelection();
        if (dragMode == ScreenshotSelectionDragMode::Marquee ||
            (origin.width() > 0.0 && origin.height() > 0.0)) {
            return m_context.selection.selectionRectForDrag(
                dragMode, position, m_context.geometry.canvasBounds(),
                snow_shot::presentation::kScreenshotSelectionMinimumSize,
                kEqualWidthHeightAspectRatio);
        }
    }
    return m_context.selection.selectionRectForDrag(
        dragMode, position, m_context.geometry.canvasBounds(),
        snow_shot::presentation::kScreenshotSelectionMinimumSize);
}

void ScreenshotOverlayInputHandler::finishTransientDrag() {
    m_moveDragModeBeforeShortcut = ScreenshotSelectionDragMode::None;
    m_marqueeAnchor = QPointF();
    m_lastMoveDragPosition = QPointF();
}

void ScreenshotOverlayInputHandler::restoreToolAfterSelectionResize() {
    if (!m_toolBeforeSelectionResize.has_value()) {
        return;
    }

    const ScreenshotActiveTool previousTool = *m_toolBeforeSelectionResize;
    m_toolBeforeSelectionResize.reset();
    if (!m_context.interaction.moveToolActive() ||
        !(m_context.interaction.movingSelection() ||
          m_context.interaction.modifyingSelection())) {
        return;
    }
    if (!m_context.actions.activateToolForSelectionResize(previousTool)) {
        m_toolBeforeSelectionResize = previousTool;
    }
}

void ScreenshotOverlayInputHandler::restoreScrollingCaptureAfterFailedResize() {
    if (!m_scrollingCaptureSelectionResize) {
        return;
    }
    m_scrollingCaptureSelectionResize = false;
    m_context.actions.resumeScrollingCapture();
}

void ScreenshotOverlayInputHandler::resetTransientShortcuts() {
    cancelCanvasColorSampling();
    restoreToolAfterSelectionResize();
    restoreScrollingCaptureAfterFailedResize();
    m_moveEntireSelectionShortcut = false;
    m_keepSelectionAspectRatioShortcut = false;
    m_aspectShortcutUsedForSelectionDrag = false;
    m_cycleColorFormatIfAspectShortcutUnused = false;
    finishTransientDrag();
}

bool ScreenshotOverlayInputHandler::canvasColorSamplingActive() const {
    return m_canvasColorSamplingArmed;
}

void ScreenshotOverlayInputHandler::armCanvasColorSampling() {
    m_canvasColorSamplingArmed = true;
}

void ScreenshotOverlayInputHandler::cancelCanvasColorSampling() {
    if (!m_canvasColorSamplingArmed) {
        return;
    }
    m_canvasColorSamplingArmed = false;
    m_context.actions.cancelCanvasColorSampling();
}
