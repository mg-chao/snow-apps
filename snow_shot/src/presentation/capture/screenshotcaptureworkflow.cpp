#include "snow_shot/presentation/screenshotcaptureworkflow.h"

#include "snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h"
#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshottoolcommandworkflowports.h"
#include "../services/screenshotlifecycleperfinstrumentation.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>

#include <algorithm>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
QPoint currentPhysicalCursorPosition(const ScreenshotDisplaySession& displaySession,
                                     const ScreenshotGeometryMapper& geometry) {
    if (!geometry.isEmpty()) {
        return geometry.physicalPositionForLogicalPoint(displaySession, QCursor::pos());
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    POINT cursor{};
    if (GetCursorPos(&cursor) != FALSE) {
        return QPoint(cursor.x, cursor.y);
    }
#endif

    return geometry.physicalPositionForLogicalPoint(displaySession, QCursor::pos());
}
} // namespace

ScreenshotCaptureWorkflow::ScreenshotCaptureWorkflow(ScreenshotCaptureWorkflowContext context)
    : m_context(std::move(context)), m_state(m_context.state) {
    m_context.runtime.setEventSink(this);
}
ScreenshotCaptureWorkflow::~ScreenshotCaptureWorkflow() {
    m_context.runtime.setEventSink(nullptr);
}

void ScreenshotCaptureWorkflow::prewarmResources() {
    if (m_state.captureInProgress || !m_context.interaction.inactive()) {
        return;
    }
    if (m_state.sessionState == ScreenshotSessionState::IdlePrepared) {
        return;
    }

    initializeIdleResources(0);
}

void ScreenshotCaptureWorkflow::startCapture(
    ScreenshotCapturePresentationMode presentationMode, quintptr focusedWindowHandle) {
    bool reusePriorCleanup = false;
    const bool coldStart = m_state.sessionState == ScreenshotSessionState::IdleCold;
    if (m_state.sessionState != ScreenshotSessionState::IdleCold &&
        m_state.sessionState != ScreenshotSessionState::IdlePrepared) {
        cleanupActiveSessionForRestart();
        reusePriorCleanup = true;
    }
    if (!m_context.runtime.captureWorkerCreated()) {
        m_context.runtime.ensureCaptureWorker();
    }
    const quint64 sessionId = ++m_state.sessionId;
    m_presentationMode = presentationMode;
    m_focusedWindowHandle = focusedWindowHandle;
    m_state.sessionState = ScreenshotSessionState::Capturing;
    m_state.captureInProgress = true;
    clearCapturePresentationReadiness();
    if (coldStart && !reusePriorCleanup) {
        resetCaptureModels();
        resetCanvasRuntimeState();
    }
    m_captureModelsClean = false;
    m_context.interaction.beginCapture();
    m_context.intelligentSelection.beginCaptureSession(m_context.smartSelectionEnabled());
    beginCapturePreparation(sessionId);
}

void ScreenshotCaptureWorkflow::handleInitialSmartSelectionResolved(quint64 sessionId) {
    if (sessionId != m_state.sessionId || m_initialSmartSelectionPendingSessionId != sessionId) {
        return;
    }

    m_initialSmartSelectionResolvedSessionId = sessionId;
    showCapturePresentationWhenReady(sessionId);
}

void ScreenshotCaptureWorkflow::cancelCapture() {
    m_context.runtime.cancelActiveCapture();
    if (m_context.captureTerminated) {
        m_context.captureTerminated();
    }
    ++m_state.sessionId;
    m_state.sessionState = ScreenshotSessionState::Releasing;
    m_state.captureInProgress = false;
    m_focusedWindowHandle = 0;
    resetCaptureModels();
    resetCanvasRuntimeState();
    releaseIdleResources(m_state.sessionId);
}

void ScreenshotCaptureWorkflow::clearCapturePresentationReadiness() {
    m_capturedPresentationSessionId = 0;
    m_initialSmartSelectionPendingSessionId = 0;
    m_initialSmartSelectionResolvedSessionId = 0;
    m_visiblePresentationSessionId = 0;
}

void ScreenshotCaptureWorkflow::resetCaptureModels() {
    clearCapturePresentationReadiness();

    m_context.interaction.reset();
    m_context.selection.reset();
    m_context.intelligentSelection.reset();
    m_context.runtime.resetColorPicker();
    m_captureModelsClean = true;
}

void ScreenshotCaptureWorkflow::clearDisplays() {
    clearCapturePresentationReadiness();
    m_context.runtime.clearDisplays(m_context.displaySession);
    m_context.geometry.clear();
}

void ScreenshotCaptureWorkflow::destroyDisplayPool() {
    clearCapturePresentationReadiness();
    m_context.runtime.destroyDisplayPool(m_context.displaySession);
    m_context.displaySession.clear();
    m_context.geometry.clear();
}

void ScreenshotCaptureWorkflow::cleanupActiveSessionForRestart() {
    m_context.runtime.cancelActiveCapture();
    if (m_context.captureTerminated) {
        m_context.captureTerminated();
    }
    ++m_state.sessionId;
    m_state.sessionState = ScreenshotSessionState::Releasing;
    m_state.captureInProgress = false;
    m_context.runtime.releaseSelectorCache();
    resetCaptureModels();
    m_context.runtime.hideOverlayWindows(m_context.displaySession);
    if (m_context.presentation.hideToolbar) {
        m_context.presentation.hideToolbar();
    }
    clearDisplays();
    m_context.runtime.resetForNewCapture(m_context.displaySession);
    resetCanvasRuntimeState();
}

void ScreenshotCaptureWorkflow::destroyUiSelectorService() {
    m_context.runtime.destroySelectorService();
}

void ScreenshotCaptureWorkflow::shutdownCaptureWorker() {
    m_context.runtime.shutdownCaptureWorker();
}

void ScreenshotCaptureWorkflow::releaseIdleResources(quint64 sessionId) {
    if (sessionId != m_state.sessionId || m_state.captureInProgress ||
        !m_context.interaction.inactive()) {
        return;
    }

    m_state.sessionState = ScreenshotSessionState::Releasing;
    m_context.runtime.destroySelectorService();
    m_context.runtime.hideOverlayWindows(m_context.displaySession);
    if (m_context.presentation.hideToolbar) {
        m_context.presentation.hideToolbar();
    }
    destroyDisplayPool();
    m_context.runtime.shutdownCaptureWorker();
    m_state.sessionState = ScreenshotSessionState::IdleCold;
}

void ScreenshotCaptureWorkflow::releaseResourcesForExternalInvalidation() {
    if (!m_context.interaction.inactive() || m_state.captureInProgress) {
        cancelCapture();
        return;
    }
    releaseIdleResources(m_state.sessionId);
}

void ScreenshotCaptureWorkflow::handleDisplayConfigurationChanged() {
    if (!m_context.runtime.captureWorkerCreated() &&
        m_state.sessionState == ScreenshotSessionState::IdleCold &&
        m_context.displaySession.isEmpty()) {
        return;
    }

    if (m_context.runtime.captureWorkerCreated()) {
        m_state.layoutDirty = true;
    }
    releaseResourcesForExternalInvalidation();
}

void ScreenshotCaptureWorkflow::beginCapturePreparation(quint64 sessionId) {
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.capture_prepare_begin"));
    if (!m_context.runtime.captureWorkerCreated()) {
        m_context.runtime.ensureCaptureWorker();
    }
    if (m_presentationMode == ScreenshotCapturePresentationMode::Silent) {
        m_context.runtime.captureAsync(
            ScreenshotCaptureRequest{sessionId, m_state.layoutDirty, m_focusedWindowHandle});
        return;
    }

    static_cast<void>(
        m_context.runtime.preparePreCaptureOverlayWindows(m_context.displaySession));
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.overlay_exclusions_ready"));
    // Once Snow Shot's windows are excluded, start native acquisition at
    // once. Selector and canvas initialization wait for a validated capture
    // result so cancellation can remain a lightweight path.
    m_context.runtime.captureAsync(
        ScreenshotCaptureRequest{sessionId, m_state.layoutDirty, m_focusedWindowHandle});
}

void ScreenshotCaptureWorkflow::finishCapturePreparation(const ScreenshotCaptureResult& result) {
    const quint64 sessionId = result.requestId;
    if (sessionId != m_state.sessionId || !m_state.captureInProgress) {
        return;
    }
    if (!result.succeeded || result.displays.isEmpty() ||
        (m_focusedWindowHandle != 0 && !result.focusedWindow.has_value())) {
        if (!result.errorMessage.isEmpty()) {
            qWarning("Screenshot capture failed: %s", qPrintable(result.errorMessage));
        }
        cancelCapture();
        return;
    }
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.result_received"));

    m_context.focusedWindowCaptured(result.focusedWindow);
    m_focusedWindowHandle = 0;

    m_context.geometry.clear();
    ScreenshotCaptureDisplayModelReconciler::applySnapshots(m_context.displaySession,
                                                            result.displays);
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.display_models_applied"));

    if (!m_context.displaySession.hasActiveDisplays()) {
        cancelCapture();
        return;
    }
    m_context.geometry.rebuild(m_context.displaySession);
    m_state.layoutDirty = false;

    m_state.captureInProgress = false;
    if (m_presentationMode == ScreenshotCapturePresentationMode::Silent) {
        m_state.sessionState = ScreenshotSessionState::OverlayVisible;
        m_capturedPresentationSessionId = sessionId;
        if (m_context.presentation.capturePresented) {
            m_context.presentation.capturePresented();
        }
        return;
    }
    m_state.sessionState = ScreenshotSessionState::OverlayVisible;
    enterOverlaySelectionModeAtCursor();
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.interaction_ready"));

    m_context.runtime.applyDisplayModels(m_context.displaySession);
    m_canvasRuntimeClean = false;

    m_capturedPresentationSessionId = sessionId;
    showCapturePresentationWhenReady(sessionId);
}

void ScreenshotCaptureWorkflow::showCapturePresentationWhenReady(quint64 sessionId) {
    if (sessionId != m_state.sessionId || m_capturedPresentationSessionId != sessionId ||
        m_visiblePresentationSessionId == sessionId ||
        !m_context.displaySession.hasActiveDisplays()) {
        return;
    }
    m_visiblePresentationSessionId = sessionId;

    // Native pixels are the only prerequisite for the first frame. Smart
    // selection refines the already-visible overlay when its asynchronous
    // hit-test finishes; blocking on it makes the screenshot command feel
    // stalled even though the captured desktop is ready to display.
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.show_begin"));
    m_context.runtime.showOverlayWindows(m_context.displaySession,
                                         ScreenshotOverlayShowMode::CapturedImage);
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.show_complete"));
    if (m_context.presentation.updateColorPicker) {
        m_context.presentation.updateColorPicker();
    }
    if (m_context.presentation.capturePresented) {
        m_context.presentation.capturePresented();
    }
    // Publish the visible frame before the Windows foreground negotiation. The
    // latter is required for reliable mouse/keyboard routing, but can block on
    // the compositor for a frame and must not delay screenshot presentation.
    m_context.runtime.activateOverlayWindows(m_context.displaySession, []() {
        snow_shot::presentation::screenshot_lifecycle_perf::captureInteractionReady();
    });
    // Toolbar, shortcut hints, and selection decoration are refinement work.
    // Give external input a brief chance to reach the freshly activated canvas
    // before running this cold-start-heavy pass, while carrying the session ID
    // so a canceled capture cannot apply stale state later.
    if (m_context.presentation.deferOverlayState) {
        m_context.presentation.deferOverlayState(sessionId);
    } else if (m_context.presentation.updateOverlayState) {
        m_context.presentation.updateOverlayState();
    }
    if (!m_context.runtime.selectorReady() && !m_context.runtime.selectorRefreshInFlight()) {
        m_context.runtime.startWorkflowRefresh();
    }
}

void ScreenshotCaptureWorkflow::enterOverlaySelectionModeAtCursor() {
    bool selectorReady = m_context.runtime.selectorReady();
    bool selectorRefreshInFlight = m_context.runtime.selectorRefreshInFlight();
    if (!selectorRefreshInFlight && !selectorReady) {
        m_context.runtime.startWorkflowRefresh();
        selectorRefreshInFlight = m_context.runtime.selectorRefreshInFlight();
        selectorReady = m_context.runtime.selectorReady();
    }

    const bool selectorCanRespond = selectorRefreshInFlight || selectorReady;
    m_context.interaction.enterOverlayVisible(selectorCanRespond);
    m_context.intelligentSelection.clearPress();
    m_context.runtime.clearSelectorSelection();
    if (!selectorCanRespond) {
        return;
    }

    if (!m_context.runtime.selectorHitTestInFlight()) {
        m_initialSmartSelectionPendingSessionId = m_state.sessionId;
        if (m_context.runtime.updateSelectorSelectionAt(
                currentPhysicalCursorPosition(m_context.displaySession, m_context.geometry))) {
            return;
        }
        m_initialSmartSelectionPendingSessionId = 0;
        showCapturePresentationWhenReady(m_state.sessionId);
    }
}

void ScreenshotCaptureWorkflow::handleCapturePrepared(quint64, bool ok) {
    if (ok && m_state.sessionState == ScreenshotSessionState::IdleCold) {
        m_state.sessionState = ScreenshotSessionState::IdlePrepared;
    }
}

void ScreenshotCaptureWorkflow::handleCaptureFinished(const ScreenshotCaptureResult& result) {
    finishCapturePreparation(result);
}

void ScreenshotCaptureWorkflow::prewarmOverlayPool() {
    const int screenCount = std::max(1, QGuiApplication::instance() != nullptr
                                            ? static_cast<int>(QGuiApplication::screens().size())
                                            : 0);
    m_context.runtime.prewarmDisplayPool(m_context.displaySession, screenCount);
}

void ScreenshotCaptureWorkflow::initializeIdleResources(quint64 requestId) {
    m_context.runtime.prepareAsync(requestId);
    prewarmOverlayPool();
    m_context.runtime.prewarmToolbar();
    if (!m_captureModelsClean) {
        resetCaptureModels();
    }
    m_context.runtime.resetForNewCapture(m_context.displaySession);
    if (!m_canvasRuntimeClean) {
        resetCanvasRuntimeState();
    }
    if (m_context.presentation.hideToolbar) {
        m_context.presentation.hideToolbar();
    }
    m_state.sessionState = ScreenshotSessionState::IdlePrepared;
}

void ScreenshotCaptureWorkflow::resetCanvasRuntimeState() {
    if (m_context.runtime.clearDocumentPreservingViewports()) {
        m_context.runtime.clearOverlayCanvases(m_context.displaySession);
        m_canvasRuntimeClean = true;
        return;
    }
    if (!m_context.runtime.resetCanvasRuntime() &&
        !m_context.runtime.clearDocumentPreservingViewports()) {
        qWarning("Failed to reset screenshot canvas runtime");
        m_canvasRuntimeClean = false;
        return;
    }
    m_context.runtime.clearOverlayCanvases(m_context.displaySession);
    m_canvasRuntimeClean = true;
}
