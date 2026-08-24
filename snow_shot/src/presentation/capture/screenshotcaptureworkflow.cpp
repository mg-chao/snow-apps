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

QVector<QRect> activePhysicalRects(const ScreenshotDisplaySession& displaySession) {
    QVector<QRect> rects;
    rects.reserve(static_cast<int>(displaySession.size()));
    displaySession.forEachActiveDisplay([&rects](qsizetype, const CapturedDisplayModel& display) {
        if (!display.physicalRect.isEmpty()) {
            rects.push_back(display.physicalRect);
        }
    });
    std::sort(rects.begin(), rects.end(), [](const QRect& left, const QRect& right) {
        if (left.x() != right.x()) {
            return left.x() < right.x();
        }
        if (left.y() != right.y()) {
            return left.y() < right.y();
        }
        if (left.width() != right.width()) {
            return left.width() < right.width();
        }
        return left.height() < right.height();
    });
    return rects;
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

void ScreenshotCaptureWorkflow::startCapture(ScreenshotCapturePresentationMode presentationMode,
                                             quintptr focusedWindowHandle) {
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

bool ScreenshotCaptureWorkflow::handleInitialSmartSelectionResult(
    quint64 sessionId, const QPoint& physicalPoint, bool ok,
    const QVector<QRectF>& physicalHitRects) {
    if (sessionId != m_state.sessionId || m_initialSmartSelectionPendingSessionId != sessionId ||
        m_visiblePresentationSessionId == sessionId) {
        return false;
    }

    const QPoint currentPoint =
        currentPhysicalCursorPosition(m_context.displaySession, m_context.geometry);
    if (physicalPoint != currentPoint &&
        m_context.runtime.updateSelectorSelectionAt(currentPoint)) {
        m_initialSmartSelectionPoint = currentPoint;
        return true;
    }

    resolveInitialSmartSelection(sessionId, physicalPoint, ok, physicalHitRects);
    return true;
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
    static_cast<void>(
        releaseIdleResourcesInternal(m_state.sessionId, m_context.retainIdleResourcesForFastRestart
                                                            ? IdleResourcePolicy::RetainWarm
                                                            : IdleResourcePolicy::Destroy));
}

void ScreenshotCaptureWorkflow::clearCapturePresentationReadiness() {
    m_capturedPresentationSessionId = 0;
    m_captureEnvironmentReadySessionId = 0;
    m_initialSmartSelectionPendingSessionId = 0;
    m_initialSmartSelectionResolvedSessionId = 0;
    m_visiblePresentationSessionId = 0;
    m_initialSmartSelectionPoint = QPoint();
    m_initialSmartSelectionPhysicalHitRects.clear();
    m_preCapturePhysicalRects.clear();
    m_initialSmartSelectionSucceeded = false;
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
    static_cast<void>(releaseIdleResourcesInternal(sessionId, IdleResourcePolicy::Destroy));
}

bool ScreenshotCaptureWorkflow::releaseRetainedIdleResources(
    std::function<void(bool released)> completion) {
    return releaseIdleResourcesInternal(m_state.sessionId, IdleResourcePolicy::Hibernate,
                                        std::move(completion));
}

bool ScreenshotCaptureWorkflow::releaseIdleResourcesInternal(
    quint64 sessionId, IdleResourcePolicy policy, std::function<void(bool released)> completion) {
    if (sessionId != m_state.sessionId || m_state.captureInProgress ||
        !m_context.interaction.inactive()) {
        return false;
    }

    m_state.sessionState = ScreenshotSessionState::Releasing;
    if (policy == IdleResourcePolicy::Destroy) {
        m_context.runtime.destroySelectorService();
    } else {
        m_context.runtime.releaseSelectorCache();
    }
    m_context.runtime.hideOverlayWindows(m_context.displaySession);
    if (m_context.presentation.hideToolbar) {
        m_context.presentation.hideToolbar();
    }
    if (policy == IdleResourcePolicy::RetainWarm) {
        // Clear captured rasters and canvas state while retaining the prepared
        // display/window pool and capture worker for a short restart window.
        clearDisplays();
        m_context.runtime.resetForNewCapture(m_context.displaySession);
        m_state.sessionState = ScreenshotSessionState::IdlePrepared;
        return true;
    }
    if (policy == IdleResourcePolicy::Hibernate) {
        // The short RetainWarm window has elapsed. Preserve the workflow/service graph, but
        // retire capture-scoped leaf objects so touched worker stacks, selector state, widgets,
        // and native surfaces return to the same cold state used before the first capture.
        m_context.runtime.destroySelectorService();
        destroyDisplayPool();
        const auto finishColdHibernation =
            [this, sessionId, completion = std::move(completion)](bool released) mutable {
                if (sessionId != m_state.sessionId || m_state.captureInProgress ||
                    !m_context.interaction.inactive()) {
                    if (completion) {
                        completion(false);
                    }
                    return;
                }
                m_context.runtime.shutdownCaptureWorker();
                m_state.sessionState = ScreenshotSessionState::IdleCold;
                if (completion) {
                    completion(released);
                }
            };
        if (m_context.runtime.releaseIdleResourcesAsync(sessionId, finishColdHibernation)) {
            return true;
        }
        m_context.runtime.shutdownCaptureWorker();
        m_state.sessionState = ScreenshotSessionState::IdleCold;
        return false;
    }
    destroyDisplayPool();
    m_context.runtime.shutdownCaptureWorker();
    m_state.sessionState = ScreenshotSessionState::IdleCold;
    return true;
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

    // The worker initializes a hibernated backend before publishing its ready
    // milestone. Native-surface restoration and selector work start from that
    // milestone while acquisition continues on the worker thread.
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
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.capture_pixels_ready"));
    if (m_presentationMode == ScreenshotCapturePresentationMode::Silent) {
        m_state.sessionState = ScreenshotSessionState::OverlayVisible;
        m_capturedPresentationSessionId = sessionId;
        if (m_context.presentation.capturePresented) {
            m_context.presentation.capturePresented();
        }
        return;
    }
    m_context.runtime.applyDisplayModels(m_context.displaySession);
    m_canvasRuntimeClean = false;

    m_capturedPresentationSessionId = sessionId;
    if (!initialSelectorGeometryMatchesCapture()) {
        retryInitialSmartSelection(sessionId);
    }
    showCapturePresentationWhenReady(sessionId);
}

void ScreenshotCaptureWorkflow::showCapturePresentationWhenReady(quint64 sessionId) {
    if (sessionId != m_state.sessionId || m_capturedPresentationSessionId != sessionId ||
        m_visiblePresentationSessionId == sessionId ||
        !m_context.displaySession.hasActiveDisplays()) {
        return;
    }
    if (m_initialSmartSelectionResolvedSessionId != sessionId) {
        return;
    }

    const QPoint currentPoint =
        currentPhysicalCursorPosition(m_context.displaySession, m_context.geometry);
    if (m_initialSmartSelectionSucceeded && m_initialSmartSelectionPoint != currentPoint) {
        m_initialSmartSelectionResolvedSessionId = 0;
        m_initialSmartSelectionSucceeded = false;
        m_initialSmartSelectionPhysicalHitRects.clear();
        m_initialSmartSelectionPoint = currentPoint;
        if (m_context.runtime.updateSelectorSelectionAt(currentPoint)) {
            return;
        }
        resolveInitialSmartSelection(sessionId, currentPoint, false, {});
        return;
    }

    bool initialSelectionApplied = false;
    if (m_initialSmartSelectionSucceeded) {
        initialSelectionApplied =
            m_context.runtime.applySelectorHitPath(m_initialSmartSelectionPhysicalHitRects);
    }
    if (!initialSelectionApplied) {
        m_context.runtime.clearSelectorSelection();
        m_context.interaction.enterOverlayVisible(false);
    }
    if (m_context.presentation.prepareInitialOverlayState) {
        m_context.presentation.prepareInitialOverlayState();
    } else if (m_context.presentation.updateOverlayState) {
        m_context.presentation.updateOverlayState();
    }
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.initial_frame_prepared"));

    m_state.sessionState = ScreenshotSessionState::OverlayVisible;
    m_visiblePresentationSessionId = sessionId;
    m_initialSmartSelectionPendingSessionId = 0;

    // Reveal only after captured pixels and the initial smart-selection result
    // have been committed. Keeping this as one presentation step prevents an
    // incomplete overlay from being visible while selector work finishes.
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.show_begin"));
    m_context.runtime.showOverlayWindows(m_context.displaySession,
                                         ScreenshotOverlayShowMode::CapturedImage);
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.show_complete"));
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.first_complete_smart_frame_presented"));
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
    // Toolbar, shortcut hints, guide lines, and color-picker decoration are
    // refinement work. The initial selection frame is already committed.
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

void ScreenshotCaptureWorkflow::beginInitialSmartSelection(quint64 sessionId) {
    if (sessionId != m_state.sessionId) {
        return;
    }

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
        resolveInitialSmartSelection(sessionId, {}, false, {});
        return;
    }

    if (!m_context.runtime.selectorHitTestInFlight()) {
        m_initialSmartSelectionPendingSessionId = sessionId;
        m_initialSmartSelectionPoint =
            currentPhysicalCursorPosition(m_context.displaySession, m_context.geometry);
        snow_shot::presentation::screenshot_lifecycle_perf::mark(
            QStringLiteral("presentation.initial_selection_begin"));
        if (m_context.runtime.updateSelectorSelectionAt(m_initialSmartSelectionPoint)) {
            return;
        }
        resolveInitialSmartSelection(sessionId, m_initialSmartSelectionPoint, false, {});
    }
}

void ScreenshotCaptureWorkflow::resolveInitialSmartSelection(
    quint64 sessionId, const QPoint& physicalPoint, bool ok,
    const QVector<QRectF>& physicalHitRects) {
    if (sessionId != m_state.sessionId) {
        return;
    }

    m_initialSmartSelectionPendingSessionId = sessionId;
    m_initialSmartSelectionResolvedSessionId = sessionId;
    m_initialSmartSelectionPoint = physicalPoint;
    m_initialSmartSelectionPhysicalHitRects = physicalHitRects;
    m_initialSmartSelectionSucceeded = ok && !physicalHitRects.isEmpty();
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.initial_selection_resolved"));
    showCapturePresentationWhenReady(sessionId);
}

void ScreenshotCaptureWorkflow::retryInitialSmartSelection(quint64 sessionId) {
    if (sessionId != m_state.sessionId) {
        return;
    }

    m_context.runtime.resetHitTestState();
    m_initialSmartSelectionResolvedSessionId = 0;
    m_initialSmartSelectionSucceeded = false;
    m_initialSmartSelectionPhysicalHitRects.clear();
    m_preCapturePhysicalRects = activePhysicalRects(m_context.displaySession);
    m_context.runtime.startWorkflowRefresh();
    beginInitialSmartSelection(sessionId);
}

bool ScreenshotCaptureWorkflow::initialSelectorGeometryMatchesCapture() const {
    if (m_preCapturePhysicalRects != activePhysicalRects(m_context.displaySession)) {
        return false;
    }
    if (m_initialSmartSelectionSucceeded &&
        m_context.geometry.displayForPhysicalPoint(m_context.displaySession,
                                                   m_initialSmartSelectionPoint) == nullptr) {
        return false;
    }
    if (m_initialSmartSelectionSucceeded) {
        const QPointF point(m_initialSmartSelectionPoint);
        const QRectF physicalBounds = m_context.geometry.physicalBounds();
        for (const QRectF& hitRect : m_initialSmartSelectionPhysicalHitRects) {
            if (!hitRect.isValid() || hitRect.isEmpty() || !hitRect.contains(point) ||
                !hitRect.intersects(physicalBounds)) {
                return false;
            }
        }
    }
    return true;
}

void ScreenshotCaptureWorkflow::handleCapturePrepared(quint64, bool ok) {
    if (ok && m_state.sessionState == ScreenshotSessionState::IdleCold) {
        m_state.sessionState = ScreenshotSessionState::IdlePrepared;
    }
}

void ScreenshotCaptureWorkflow::handleCaptureEnvironmentReady(quint64 requestId, bool ok) {
    if (!ok || requestId != m_state.sessionId || !m_state.captureInProgress ||
        m_presentationMode == ScreenshotCapturePresentationMode::Silent ||
        m_captureEnvironmentReadySessionId == requestId) {
        return;
    }
    m_captureEnvironmentReadySessionId = requestId;
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.capture_environment_ready"));

    const bool overlaysPrepared =
        m_context.runtime.preparePreCaptureOverlayWindows(m_context.displaySession);
    snow_shot::presentation::screenshot_lifecycle_perf::mark(
        QStringLiteral("presentation.overlay_exclusions_ready"));

    if (!overlaysPrepared || !m_context.displaySession.hasActiveDisplays()) {
        resolveInitialSmartSelection(requestId, {}, false, {});
        return;
    }

    m_context.geometry.rebuild(m_context.displaySession);
    m_preCapturePhysicalRects = activePhysicalRects(m_context.displaySession);
    beginInitialSmartSelection(requestId);
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
