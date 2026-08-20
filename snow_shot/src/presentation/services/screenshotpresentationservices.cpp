#include "snow_shot/presentation/screenshotpresentationservices.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotshortcuthints.h"
#include "snow_shot/presentation/screenshottoolbarpresenter.h"
#include "snow_shot/presentation/screenshottoolbarpresentationstatefactory.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "screenshotlifecycleperfinstrumentation.h"

#include <QCursor>

ScreenshotPresentationServices::ScreenshotPresentationServices(
    ScreenshotPresentationServicesContext context)
    : m_context(context), m_smartSelectionTransition([this](const QRectF& selection) {
          presentSelectionFrame(selection);
      }) {
    reloadConfiguredShortcuts();
}

void ScreenshotPresentationServices::hideToolbar() {
    m_context.toolbarPresenter.hideToolbar();
}

void ScreenshotPresentationServices::hideMainToolbar() {
    m_context.toolbarPresenter.hideMainToolbar();
}

void ScreenshotPresentationServices::showToolbar() {
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.toolbar_show_begin"));
    m_context.toolbarPresenter.showToolbar(toolbarPresentationState());
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.toolbar_show_complete"));
}

void ScreenshotPresentationServices::showSelectionToolbar() {
    m_context.toolbarPresenter.showSelectionToolbar(toolbarPresentationState());
}

void ScreenshotPresentationServices::moveToolbar() {
    m_context.toolbarPresenter.moveToolbar(toolbarPresentationState());
}

void ScreenshotPresentationServices::repositionToolbarForContentChange() {
    m_context.toolbarPresenter.repositionForContentChange(toolbarPresentationState());
}

void ScreenshotPresentationServices::raiseToolbarForCanvasInteraction() {
    m_context.toolbarPresenter.raiseToolbarForCanvasInteraction(toolbarPresentationState());
}

void ScreenshotPresentationServices::setSelectionToolbarHovered(bool hovered) {
    if (m_selectionToolbarHovered == hovered) {
        return;
    }

    m_selectionToolbarHovered = hovered;
    updateOverlayState();
}

void ScreenshotPresentationServices::setUiPreferences(
    const ScreenshotUiPreferences& preferences) {
    m_uiPreferences = preferences.normalized();
    m_smartSelectionTransition.setEnabled(
        m_uiPreferences.selectionTransitionAnimationEnabled);
    m_context.overlayCoordinator.setSelectionMaskColor(m_context.displaySession,
                                                       m_uiPreferences.selectionMaskColor);
    m_context.overlayCoordinator.setColorPickerCenterGuideLineColor(
        m_uiPreferences.colorPickerCenterGuideLineColor);
    updateOverlayState();
}

void ScreenshotPresentationServices::setQuickSelectionDisabledTools(
    const QSet<SnowCanvasTool>& tools) {
    if (m_context.quickSelectionDisabledTools == tools) {
        return;
    }
    m_context.quickSelectionDisabledTools = tools;
    updateOverlayState();
}

void ScreenshotPresentationServices::reloadConfiguredShortcuts() {
    if (!snow_shot::storage::ApplicationStorage::instance().isInitialized()) {
        m_configuredShortcuts.reset();
        return;
    }
    m_configuredShortcuts = snow_shot::storage::ScreenshotShortcutSettings().allShortcuts();
}

void ScreenshotPresentationServices::updateOverlayState() {
    const bool smartFraming = m_context.interaction.intelligentSelecting();
    const ScreenshotToolbarPresentationState toolbarState = toolbarPresentationState();
    m_context.toolbarPresenter.updateSelectionToolbarState(toolbarState, !smartFraming);
    const bool selectionChanged =
        m_smartSelectionTransition.update(m_context.selection.normalizedSelection(), smartFraming);
    if (!selectionChanged) {
        presentOverlayState(m_smartSelectionTransition.displayedSelection());
    }
}

void ScreenshotPresentationServices::presentSelectionFrame(const QRectF& selection) {
    presentOverlayState(selection);
    if (!m_context.interaction.intelligentSelecting()) {
        return;
    }

    ScreenshotToolbarPresentationState toolbarState = toolbarPresentationState();
    toolbarState.selectionCanvas = selection;
    m_context.toolbarPresenter.moveSelectionToolbar(toolbarState);
}

void ScreenshotPresentationServices::presentOverlayState(const QRectF& selection) const {
    m_context.overlayCoordinator.setSelectionMaskColor(m_context.displaySession,
                                                       m_uiPreferences.selectionMaskColor);
    m_context.overlayCoordinator.updateOverlayState(
        m_context.displaySession, selection, m_context.selection.cornerRadius(),
        m_context.selection.shadowWidth(), m_context.selection.shadowColor(),
        m_selectionToolbarHovered,
        !m_context.interaction.intelligentSelecting() &&
            m_context.interaction.selectionHandlesVisible(),
        m_context.interaction.intelligentSelecting(), m_context.interaction.marqueeSelecting(),
        m_context.interaction.dragging());

    m_context.overlayCoordinator.updateGuideLinesAtGlobalPosition(
        m_context.displaySession, QCursor::pos(), m_context.interaction.selecting(),
        m_uiPreferences.cursorGuideLineColor, m_uiPreferences.monitorCenterGuideLineColor);

    ScreenshotShortcutHintContext hintContext{
        m_context.interaction.activeTool(), m_context.interaction.mode(),
        m_context.quickSelectionDisabledTools};
    hintContext.configuredShortcuts = m_configuredShortcuts;
    hintContext.smartSelectionEnabled = m_context.intelligentSelection.smartSelectionEnabled();
    const ScreenshotShortcutHintMode hintMode =
        screenshotShortcutHintModeForContext(hintContext);
    ScreenshotOverlayWindow* hintOwner = nullptr;
    QRectF selectionGlobal;
    if (hintMode != ScreenshotShortcutHintMode::Hidden) {
        if (selection.isValid() && !selection.isEmpty()) {
            const CapturedDisplayModel* display =
                m_context.geometry.displayForCanvasRect(m_context.displaySession, selection);
            hintOwner = m_context.displaySession.overlayForDisplay(display);
            if (hintOwner != nullptr && display != nullptr) {
                const QRectF displayCanvasRect =
                    ScreenshotGeometryMapper::displayCanvasRect(*display);
                const QRectF selectionOnDisplay = selection.intersected(displayCanvasRect);
                if (selectionOnDisplay.isValid() && !selectionOnDisplay.isEmpty()) {
                    selectionGlobal = QRectF(
                        m_context.geometry.logicalPositionForCanvasPoint(
                            *display, selectionOnDisplay.topLeft()),
                        m_context.geometry.logicalPositionForCanvasPoint(
                            *display, selectionOnDisplay.bottomRight()))
                                          .normalized();
                }
            }
        }
        if (hintOwner == nullptr) {
            const QPoint cursorPosition = QCursor::pos();
            m_context.displaySession.forEachActiveOverlay(
                [&](qsizetype, const CapturedDisplayModel& display,
                    ScreenshotOverlayWindow* overlay) {
                    if (hintOwner == nullptr &&
                        display.logicalRect.contains(cursorPosition, false)) {
                        hintOwner = overlay;
                    }
                });
        }
    }
    m_context.overlayCoordinator.updateShortcutHints(hintOwner, hintContext,
                                                     m_uiPreferences.shortcutHintOpacity,
                                                     selectionGlobal);
}

void ScreenshotPresentationServices::updateOverlayCursors() const {
    const bool selecting =
        m_context.interaction.intelligentSelecting() || m_context.interaction.marqueeSelecting();
    m_context.overlayCoordinator.updateOverlayCursors(m_context.displaySession, selecting,
                                                      m_context.interaction.dragging());
}

bool ScreenshotPresentationServices::hasActiveDisplays() const {
    return m_context.displaySession.hasActiveDisplays();
}

QPoint
ScreenshotPresentationServices::physicalPositionForLogicalPoint(const QPointF& logicalPoint) const {
    return m_context.geometry.physicalPositionForLogicalPoint(m_context.displaySession,
                                                              logicalPoint);
}

ScreenshotColorPickerContext ScreenshotPresentationServices::colorPickerContext() const {
    ScreenshotColorPickerContext context;
    context.active = !m_context.interaction.inactive() &&
                     !m_context.captureState.captureInProgress &&
                     !m_context.interaction.scrollingCapture();
    context.moveToolActive = m_context.interaction.moveToolActive();
    context.intelligentSelecting = m_context.interaction.intelligentSelecting();
    context.manualSelecting = m_context.interaction.manualSelecting();
    context.movingSelection = m_context.interaction.movingSelection();
    context.dragging = m_context.interaction.dragging();
    context.selectionPixels = m_context.selection.pixelSelection();
    context.selectionCanvas = m_context.selection.normalizedSelection();
    context.dragMode = m_context.interaction.dragMode();
    return context;
}

ScreenshotToolbarPresentationState
ScreenshotPresentationServices::toolbarPresentationState() const {
    return makeScreenshotToolbarPresentationState(m_context.interaction, m_context.selection);
}
