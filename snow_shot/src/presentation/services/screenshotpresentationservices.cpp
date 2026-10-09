#include "snow_shot/presentation/screenshotpresentationservices.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotshortcuthints.h"
#include "snow_shot/presentation/screenshottoolbarpresenter.h"
#include "snow_shot/presentation/screenshottoolbarpresentationstatefactory.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QTimer>

#include <cmath>
#include <algorithm>
#include <utility>

namespace {
struct PresentationLifecycle {
    ScreenshotSessionState sessionState = ScreenshotSessionState::IdleCold;
    ScreenshotStartupContext::Phase startupPhase = ScreenshotStartupContext::Phase::Inactive;
    quint64 layoutGeneration = 0;
    quint64 displayRevision = 0;
    bool captureInProgress = false;
    bool suppressed = false;
    bool cursorAvailable = false;
    bool cursorVisible = false;
    bool operator==(const PresentationLifecycle&) const = default;
};
PresentationLifecycle presentationLifecycle(const ScreenshotPresentationServicesContext& context) {
    const auto& startup = context.displaySession.startup;
    return {context.captureState.sessionState,
            startup ? startup->phase : ScreenshotStartupContext::Phase::Inactive,
            startup ? startup->layoutGeneration : 0,
            context.overlayCoordinator.presentationRevision(),
            context.captureState.captureInProgress,
            context.captureState.presentationSuppressed,
            context.displaySession.cursorAvailable,
            context.displaySession.cursorVisible};
}

ScreenshotSelectionVisualState
selectionVisualState(const ScreenshotPresentationServicesContext& context, bool toolbarHovered,
                     const QRectF& selection) {
    const bool regionOperation = context.selection.regionOperationActive();
    const bool shaped = regionOperation || context.selection.selectionRegion().rectCount() > 1;
    ScreenshotSelectionVisualState visualState;
    visualState.bounds =
        regionOperation
            ? QRectF(context.selection.confirmedRegion().boundingRect()).united(selection)
            : selection;
    visualState.present = visualState.bounds.isValid() && !visualState.bounds.isEmpty();
    visualState.handlesVisible = !context.interaction.intelligentSelecting() &&
                                 context.interaction.selectionHandlesVisible() &&
                                 context.selection.rectangular();
    visualState.cornerRadius = context.selection.cornerRadius();
    visualState.shadowWidth = context.selection.shadowWidth();
    visualState.shadowColor = context.selection.shadowColor();
    visualState.toolbarHovered = toolbarHovered;
    visualState.effectEditorsVisible =
        context.interaction.movingSelection() && context.interaction.moveToolActive() &&
        context.selection.rectangular() && context.selection.cornerRadiusApplicable() &&
        !toolbarHovered;
    visualState.hoveredEffectHandle = context.interaction.hoveredEffectHandle();
    if (context.interaction.effectGesture())
        visualState.activeEffectHandle = context.interaction.effectGesture()->handle;
    visualState.effectPreviewVisible =
        visualState.effectEditorsVisible &&
        (visualState.hoveredEffectHandle == ScreenshotSelectionEffectHandle::Shadow ||
         visualState.activeEffectHandle == ScreenshotSelectionEffectHandle::Shadow);
    visualState.draftPath = context.selection.draftPath();
    visualState.draftVertices = context.selection.draftVertices();
    if (shaped) {
        const bool animatedMarquee = context.interaction.intelligentSelecting() &&
                                     regionOperation && !context.selection.constructionActive();
        visualState.region = animatedMarquee
                                 ? context.selection.selectionRegionForMarquee(selection)
                                 : context.selection.selectionRegion();
        visualState.confirmedRegion = context.selection.confirmedRegion();
        visualState.marquee = animatedMarquee ? selection : context.selection.pendingMarquee();
        visualState.subtracting = context.selection.regionOperation() ==
                                  ScreenshotSelectionModel::RegionOperation::Subtract;
        visualState.dangerColor =
            snow_shot::presentation::styles::generateThemeColorScheme().map.colorError;
        visualState.bounds = QRectF(visualState.region->boundingRect())
                                 .united(QRectF(visualState.confirmedRegion.boundingRect()))
                                 .united(visualState.marquee);
        visualState.present = !visualState.bounds.isEmpty();
        visualState.handlesVisible = false;
    }
    return visualState;
}
} // namespace

namespace {
struct PresentationSnapshot {
    ScreenshotToolbarPresentationState toolbar;
    ScreenshotSelectionVisualState visual;
    ScreenshotShortcutHintContext hints;
    PresentationLifecycle lifecycle;
    bool operator==(const PresentationSnapshot&) const = default;
};
} // namespace

struct ScreenshotPresentationServices::State {
    QElapsedTimer clock;
    QTimer timer;
    ScreenshotToolbarPresentationState toolbar;
    PresentationLifecycle lifecycle;
    ScreenshotSelectionVisualState visual;
    ScreenshotShortcutHintContext hints;
    std::optional<PresentationSnapshot> committed;
    ScreenshotShortcutHintMode hintMode = ScreenshotShortcutHintMode::Hidden;
    QPointer<ScreenshotOverlayWindow> pointerOwner;
    QPointer<ScreenshotOverlayWindow> selectionOwner;
    QPointer<ScreenshotOverlayWindow> hintOwner;
    QRectF selectionGlobal;
    QPoint cursorPosition;
    QPointF pointerLocal;
    QRect pointerCaptureGeometry;
    quint64 sessionId = 0;
    qint64 lastFrameNs = 0;
    bool initialized = false;
    bool preferencesDirty = true;
    bool presentationDirty = false;
    bool semanticDirty = false;
    bool geometryDirty = false;
    bool pointerDirty = false;
    bool pointerKnown = false;
    bool inFrame = false;
};

ScreenshotPresentationServices::ScreenshotPresentationServices(
    ScreenshotPresentationServicesContext context)
    : m_context(std::move(context)), m_state(std::make_unique<State>()),
      m_smartSelectionTransition([this](const QRectF&) { m_state->geometryDirty = true; }) {
    m_state->clock.start();
    m_state->timer.setObjectName(QStringLiteral("screenshotPresentationFrameTimer"));
    m_state->timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&m_state->timer, &QTimer::timeout, &m_state->timer, [this] {
        if (!m_state->semanticDirty && !m_state->geometryDirty && !m_state->pointerDirty &&
            !m_smartSelectionTransition.isRunning()) {
            // Keep the native timer across input bursts. On Windows, tearing
            // down a precision timer synchronizes with its callback thread.
            constexpr qint64 kIdleGraceNs = 50000000;
            if (nowNanoseconds() - m_state->lastFrameNs >= kIdleGraceNs)
                m_state->timer.stop();
            return;
        }
        flushPendingFrame();
    });
    reloadConfiguredShortcuts();
}

ScreenshotPresentationServices::~ScreenshotPresentationServices() = default;

void ScreenshotPresentationServices::hideToolbar() {
    m_context.toolbarPresenter.hideToolbar();
}

void ScreenshotPresentationServices::hideMainToolbar() {
    m_context.toolbarPresenter.hideMainToolbar();
}

void ScreenshotPresentationServices::showToolbar() {
    if (m_context.captureState.presentationSuppressed)
        return;
    m_context.toolbarPresenter.showToolbar(toolbarPresentationState());
}

void ScreenshotPresentationServices::showSelectionToolbar() {
    if (m_context.captureState.presentationSuppressed)
        return;
    m_context.toolbarPresenter.showSelectionToolbar(toolbarPresentationState());
}

void ScreenshotPresentationServices::moveToolbar() {
    if (m_selectionMovementActive)
        return;
    m_context.toolbarPresenter.moveToolbar(toolbarPresentationState());
}

void ScreenshotPresentationServices::repositionToolbarForContentChange() {
    if (m_selectionMovementActive)
        return;
    m_context.toolbarPresenter.repositionForContentChange(toolbarPresentationState());
}

void ScreenshotPresentationServices::raiseToolbarForCanvasInteraction() {
    if (m_context.captureState.presentationSuppressed)
        return;
    m_context.toolbarPresenter.raiseToolbarForCanvasInteraction(toolbarPresentationState());
}

void ScreenshotPresentationServices::setSelectionToolbarHovered(bool hovered) {
    if (m_selectionToolbarHovered == hovered) {
        return;
    }

    m_selectionToolbarHovered = hovered;
    m_context.interaction.setEffectEditorsSuppressed(hovered);
    updateOverlayState();
}

void ScreenshotPresentationServices::setUiPreferences(const ScreenshotUiPreferences& preferences) {
    const auto normalized = preferences.normalized();
    if (m_uiPreferences == normalized && !m_state->preferencesDirty)
        return;
    m_uiPreferences = normalized;
    m_state->preferencesDirty = true;
    m_smartSelectionTransition.setEnabled(m_uiPreferences.selectionTransitionAnimationEnabled);
    updateOverlayState();
}

void ScreenshotPresentationServices::setGuideLinesVisible(bool visible) {
    if (m_guideLinesVisible == visible) {
        return;
    }
    m_guideLinesVisible = visible;
    m_state->presentationDirty = true;
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
    if (m_state->initialized)
        updateOverlayState();
}

void ScreenshotPresentationServices::setSelectionMovementActive(bool active) {
    if (m_selectionMovementActive == active)
        return;
    m_selectionMovementActive = active;
    m_state->presentationDirty = true;
    if (m_state->initialized)
        updateOverlayState();
}

qint64 ScreenshotPresentationServices::nowNanoseconds() const {
    return m_context.monotonicNanoseconds ? m_context.monotonicNanoseconds()
                                          : m_state->clock.nsecsElapsed();
}

void ScreenshotPresentationServices::updateOverlayState() {
    const auto lifecycle = presentationLifecycle(m_context);
    const bool lifecycleChanged = lifecycle != m_state->lifecycle;
    if (lifecycle.displayRevision != m_state->lifecycle.displayRevision)
        m_state->preferencesDirty = true;
    const bool topologyChanged =
        lifecycle.layoutGeneration != m_state->lifecycle.layoutGeneration ||
        (m_state->pointerOwner &&
         m_state->pointerCaptureGeometry != m_state->pointerOwner->captureGeometry());
    if (topologyChanged) {
        m_state->pointerOwner.clear();
        m_state->pointerKnown = false;
        m_state->presentationDirty = true;
    }
    const auto toolbar = toolbarPresentationState();
    const auto visual = selectionVisualState(m_context, m_selectionToolbarHovered,
                                             m_context.selection.normalizedSelection());
    ScreenshotShortcutHintContext hints{m_context.interaction.activeTool(),
                                        m_context.interaction.mode(),
                                        m_context.quickSelectionDisabledTools};
    hints.configuredShortcuts = m_configuredShortcuts;
    hints.smartSelectionEnabled = m_context.intelligentSelection.smartSelectionEnabled();
    const bool firstSelection =
        !m_state->toolbar.selectionCanvas.isValid() && toolbar.selectionCanvas.isValid();
    const bool newSession =
        !m_state->initialized || m_state->sessionId != m_context.captureState.sessionId;
    const bool modeChanged =
        !m_state->initialized || hints.captureMode != m_state->hints.captureMode;
    if (!newSession && toolbar == m_state->toolbar && visual == m_state->visual &&
        hints == m_state->hints && !lifecycleChanged && !m_state->preferencesDirty &&
        !m_state->presentationDirty) {
        return;
    }
    if (newSession) {
        m_state->timer.stop();
        // Reset the trajectory as well as the pending work across capture epochs.
        (void)m_smartSelectionTransition.update({}, false, nowNanoseconds() / 1000000);
        m_state->pointerKnown = false;
        m_state->pointerDirty = false;
        m_state->pointerOwner.clear();
        m_state->preferencesDirty = true;
        m_state->committed.reset();
    }
    m_state->lifecycle = lifecycle;
    m_state->toolbar = toolbar;
    m_state->visual = visual;
    m_state->hints = std::move(hints);
    m_state->hintMode = screenshotShortcutHintModeForContext(m_state->hints);
    m_state->sessionId = m_context.captureState.sessionId;
    m_state->initialized = true;
    m_state->semanticDirty = true;
    // Reveal the first result and mode boundaries synchronously. Hover results share
    // the display frame boundary with pointer movement and animation sampling.
    if (newSession || modeChanged || firstSelection || lifecycleChanged ||
        !m_context.interaction.intelligentSelecting()) {
        flushPendingFrame();
    } else {
        scheduleFrame();
    }
}

void ScreenshotPresentationServices::updatePointerPresentation(ScreenshotOverlayWindow* overlay,
                                                               const QPointF& localPosition) {
    if (!overlay || m_context.interaction.inactive())
        return;
    const QPoint position = overlay->captureGeometry().topLeft() +
                            QPoint(qFloor(localPosition.x()), qFloor(localPosition.y()));
    if (m_state->pointerKnown && m_state->pointerLocal == localPosition &&
        m_state->pointerOwner == overlay &&
        m_state->pointerCaptureGeometry == overlay->captureGeometry())
        return;
    m_state->pointerKnown = true;
    m_state->cursorPosition = position;
    m_state->pointerLocal = localPosition;
    m_state->pointerCaptureGeometry = overlay->captureGeometry();
    if (m_state->pointerOwner != overlay)
        m_state->pointerOwner = overlay;
    m_state->pointerDirty = true;
    scheduleFrame();
}

void ScreenshotPresentationServices::scheduleFrame() {
    if (m_state->inFrame)
        return;
    QScreen* screen = m_state->pointerOwner ? m_state->pointerOwner->screen() : nullptr;
    if (!screen) {
        const auto* display = m_context.geometry.displayForCanvasRect(
            m_context.displaySession, m_state->toolbar.selectionCanvas);
        if (display)
            screen = display->screen;
    }
    const qreal rate = screen ? screen->refreshRate() : 60.0;
    const qreal validRate = std::isfinite(rate) && rate > 1.0 ? rate : 60.0;
    const qint64 period = qRound64(1000000000.0 / validRate);
    const int interval = static_cast<int>(std::max<qint64>(1, (period + 999999) / 1000000));
    if (m_state->timer.isActive()) {
        if (m_state->timer.interval() != interval)
            m_state->timer.setInterval(interval);
        return;
    }
    const qint64 remaining = m_state->lastFrameNs + period - nowNanoseconds();
    m_state->timer.start(interval);
    if (remaining <= 0) {
        const quint64 sessionId = m_state->sessionId;
        QMetaObject::invokeMethod(
            &m_state->timer,
            [this, sessionId] {
                if (m_state->sessionId == sessionId)
                    flushPendingFrame();
            },
            Qt::QueuedConnection);
    }
}

void ScreenshotPresentationServices::flushPendingFrame() {
    if (m_state->inFrame || !m_state->initialized)
        return;
    if (m_state->sessionId != m_context.captureState.sessionId) {
        m_state->timer.stop();
        (void)m_smartSelectionTransition.update({}, false, nowNanoseconds() / 1000000);
        m_state->semanticDirty = false;
        m_state->presentationDirty = false;
        m_state->geometryDirty = false;
        m_state->pointerDirty = false;
        m_state->pointerKnown = false;
        m_state->pointerOwner.clear();
        m_state->initialized = false;
        m_state->committed.reset();
        return;
    }
    if (m_state->hints.captureMode != m_context.interaction.mode() ||
        m_state->lifecycle != presentationLifecycle(m_context)) {
        updateOverlayState();
        return;
    }
    m_state->inFrame = true;
    const qint64 now = nowNanoseconds();
    const PresentationSnapshot snapshot{m_state->toolbar, m_state->visual, m_state->hints,
                                        m_state->lifecycle};
    const bool requested = std::exchange(m_state->semanticDirty, false);
    const bool notify = requested && (!m_state->committed || snapshot != *m_state->committed);
    const bool semantic =
        requested && (notify || m_state->preferencesDirty || m_state->presentationDirty);
    if (semantic)
        m_state->presentationDirty = false;
    const bool pointer = std::exchange(m_state->pointerDirty, false);
    if (semantic)
        m_state->committed = snapshot;
    if (semantic) {
        if (std::exchange(m_state->preferencesDirty, false)) {
            m_context.overlayCoordinator.setSelectionBorderColor(
                m_context.displaySession, m_uiPreferences.selectionBorderColor);
            m_context.overlayCoordinator.setSelectionMaskColor(m_context.displaySession,
                                                               m_uiPreferences.selectionMaskColor);
            m_context.overlayCoordinator.setColorPickerCenterGuideLineColor(
                m_uiPreferences.colorPickerCenterGuideLineColor);
        }
        if (!m_selectionMovementActive)
            m_context.toolbarPresenter.updateSelectionToolbarState(
                m_state->toolbar, !m_context.interaction.intelligentSelecting());
        (void)m_smartSelectionTransition.update(m_state->toolbar.selectionCanvas,
                                                m_context.interaction.intelligentSelecting(),
                                                now / 1000000);
    }
    (void)m_smartSelectionTransition.advance(now / 1000000);
    const bool geometry = std::exchange(m_state->geometryDirty, false);
    const QRectF selection = m_smartSelectionTransition.displayedSelection();
    if (semantic || geometry || pointer) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("overlay.present_state");
        presentOverlayState(selection, semantic, geometry);
        if ((semantic || geometry) && m_context.interaction.intelligentSelecting() &&
            !m_selectionMovementActive) {
            auto toolbar = m_state->toolbar;
            toolbar.selectionCanvas = selection;
            m_context.toolbarPresenter.moveSelectionToolbar(toolbar);
        }
        m_state->lastFrameNs = now;
    }
    // Side effects observe committed semantic changes; animation and pointer frames
    // never restart recognition or rebuild external interaction state.
    if (notify)
        m_context.stateChanged();
    m_state->inFrame = false;
    if (m_state->semanticDirty || m_state->geometryDirty || m_state->pointerDirty ||
        m_smartSelectionTransition.isRunning())
        scheduleFrame();
}

void ScreenshotPresentationServices::presentOverlayState(const QRectF& selection,
                                                         bool semanticChanged,
                                                         bool geometryChanged) {
    const bool anchored = m_context.displaySession.anchoredCursorPosition().has_value();
    const QPoint cursorPosition = m_state->pointerKnown && !anchored
                                      ? m_state->cursorPosition
                                      : m_context.displaySession.logicalCursorPosition();
    if (!m_state->pointerKnown && !anchored) {
        m_state->cursorPosition = cursorPosition;
        m_state->pointerKnown = true;
    }
    ScreenshotOverlayWindow* cursorOwner =
        anchored ? m_context.displaySession.startupOverlay() : m_state->pointerOwner.data();
    if (!cursorOwner ||
        !m_context.geometry.displayForOverlay(m_context.displaySession, cursorOwner))
        cursorOwner = m_context.displaySession.overlayForDisplay(
            m_context.geometry.displayForLogicalPoint(m_context.displaySession, cursorPosition));
    if (semanticChanged || geometryChanged) {
        ScreenshotSelectionVisualState visualState = m_state->visual;
        visualState.bounds = selection;
        if (m_context.interaction.intelligentSelecting() &&
            m_context.selection.regionOperationActive() &&
            !m_context.selection.constructionActive()) {
            visualState.region = m_context.selection.selectionRegionForMarquee(selection);
            visualState.marquee = selection;
            visualState.bounds = QRectF(visualState.region->boundingRect())
                                     .united(QRectF(visualState.confirmedRegion.boundingRect()))
                                     .united(selection);
        } else if (visualState.region) {
            visualState.bounds = m_state->visual.bounds;
        }
        visualState.present = visualState.bounds.isValid() && !visualState.bounds.isEmpty();
        SNOW_SHOT_CAPTURE_PERF_SCOPE("overlay.canvas_state");
        m_context.overlayCoordinator.updateOverlayState(
            m_context.displaySession, visualState, m_context.interaction.intelligentSelecting(),
            m_context.interaction.marqueeSelecting(), m_context.interaction.dragging(),
            semanticChanged);
        m_state->selectionOwner.clear();
        m_state->selectionGlobal = {};
        if (selection.isValid() && !selection.isEmpty()) {
            const CapturedDisplayModel* display =
                m_context.geometry.displayForCanvasRect(m_context.displaySession, selection);
            m_state->selectionOwner = m_context.displaySession.overlayForDisplay(display);
            if (m_state->selectionOwner && display) {
                const QRectF displayCanvasRect =
                    ScreenshotGeometryMapper::displayCanvasRect(*display);
                const QRectF selectionOnDisplay = selection.intersected(displayCanvasRect);
                if (selectionOnDisplay.isValid() && !selectionOnDisplay.isEmpty()) {
                    m_state->selectionGlobal =
                        QRectF(m_context.geometry.logicalPositionForCanvasPoint(
                                   *display, selectionOnDisplay.topLeft()),
                               m_context.geometry.logicalPositionForCanvasPoint(
                                   *display, selectionOnDisplay.bottomRight()))
                            .normalized();
                }
            }
        }
    }
    const bool preselection = m_context.interaction.preselectionActive(m_context.selection);
    if (semanticChanged || geometryChanged ||
        (preselection && m_uiPreferences.screenshotAreaTypeHintEnabled)) {
        m_context.displaySession.forEachOverlay([&](qsizetype, ScreenshotOverlayWindow* overlay) {
            if (overlay)
                overlay->setRegionTypeControlVisible(
                    m_uiPreferences.screenshotAreaTypeHintEnabled && overlay == cursorOwner &&
                        preselection,
                    m_context.selection.regionType(), m_state->selectionGlobal, cursorPosition);
        });
    }
    if (semanticChanged || m_guideLinesVisible || m_state->pointerOwner) {
        const QPointF localPosition =
            cursorOwner && cursorOwner == m_state->pointerOwner ? m_state->pointerLocal
            : cursorOwner ? QPointF(cursorOwner->canvasLocalPosition(cursorPosition))
                          : QPointF();
        m_context.overlayCoordinator.updateGuideLines(
            m_context.displaySession, cursorOwner, localPosition,
            !m_context.interaction.inactive() && m_guideLinesVisible,
            m_uiPreferences.cursorGuideLineColor, m_uiPreferences.monitorCenterGuideLineColor,
            m_uiPreferences.selectionCenterGuideLineColor);
    }

    const auto& hintContext = m_state->hints;
    const auto hintMode = m_state->hintMode;
    ScreenshotOverlayWindow* hintOwner = nullptr;
    if (hintMode != ScreenshotShortcutHintMode::Hidden) {
        hintOwner = m_state->selectionOwner.data();
        if (hintOwner == nullptr)
            hintOwner = cursorOwner;
    }
    SNOW_SHOT_CAPTURE_PERF_SCOPE("overlay.shortcut_hints");
    if (semanticChanged || geometryChanged || hintOwner != m_state->hintOwner) {
        m_context.overlayCoordinator.updateShortcutHints(hintOwner, hintContext,
                                                         m_uiPreferences.shortcutHintOpacity,
                                                         m_state->selectionGlobal, cursorPosition);
        m_state->hintOwner = hintOwner;
    } else if (hintOwner) {
        m_context.overlayCoordinator.updateShortcutHintPointer(hintOwner, cursorPosition);
    }
}

void ScreenshotPresentationServices::updateOverlayCursors() const {
    const bool selecting =
        m_context.interaction.intelligentSelecting() || m_context.interaction.marqueeSelecting();
    m_context.overlayCoordinator.updateOverlayCursors(m_context.displaySession, selecting,
                                                      m_context.interaction.dragging());
}

ScreenshotColorPickerContext ScreenshotPresentationServices::colorPickerContext() const {
    ScreenshotColorPickerContext context;
    context.selectionDisplayUnit = m_uiPreferences.selectionDisplayUnit;
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
    auto state = makeScreenshotToolbarPresentationState(m_context.interaction, m_context.selection);
    state.selectionDisplayUnit = m_uiPreferences.selectionDisplayUnit;
    return state;
}
