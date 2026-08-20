#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h"
#include "snow_shot/presentation/screenshotcaptureworkflow.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class CaptureRuntime final : public ScreenshotCaptureRuntimePort {
  public:
    void setEventSink(ScreenshotCaptureWorkerEventSink* sink) override {
        eventSink = sink;
    }

    [[nodiscard]] bool captureWorkerCreated() const override {
        return workerCreated;
    }
    [[nodiscard]] bool hasCaptureWorker() const override {
        return workerCreated;
    }
    void ensureCaptureWorker() override {
        ++ensureCaptureWorkerCalls;
        workerCreated = true;
    }
    void prepareAsync(quint64) override {
        ++prepareAsyncCalls;
        prepareAsyncOrder = ++lifecycleOperationOrder;
    }
    void captureAsync(const ScreenshotCaptureRequest& request) override {
        ++captureAllAsyncCalls;
        lastCaptureRequest = request;
        captureWasQueuedBeforeSelectorRefresh = !selectorRefreshActive;
        if (failCaptureSynchronously && eventSink != nullptr) {
            ScreenshotCaptureResult result;
            result.requestId = request.requestId;
            result.errorMessage = QStringLiteral("Synchronous capture setup failure");
            eventSink->handleCaptureFinished(result);
        }
    }
    void cancelActiveCapture() override {
        ++cancelActiveCaptureCalls;
    }
    void releaseIdleResourcesAsync(quint64) override {
        ++releaseIdleResourcesCalls;
        releaseIdleResourcesOrder = ++lifecycleOperationOrder;
    }
    void shutdownCaptureWorker() override {
        ++shutdownCaptureWorkerCalls;
        workerCreated = false;
    }

    [[nodiscard]] bool selectorReady() const override {
        return selectorIsReady;
    }
    [[nodiscard]] bool selectorRefreshInFlight() const override {
        return selectorRefreshActive;
    }
    [[nodiscard]] bool selectorHitTestInFlight() const override {
        return selectorHitTestActive;
    }
    void releaseSelectorCache() override {
        ++releaseSelectorCacheCalls;
        selectorIsReady = false;
        selectorRefreshActive = false;
    }
    void resetHitTestState() override {
        ++resetHitTestStateCalls;
        selectorHitTestActive = false;
    }
    void destroySelectorService() override {
        ++destroySelectorServiceCalls;
        selectorIsReady = false;
        selectorRefreshActive = false;
    }
    void startWorkflowRefresh() override {
        ++startWorkflowRefreshCalls;
        selectorIsReady = false;
        selectorRefreshActive = true;
    }
    void clearSelectorSelection() override {
        ++clearSelectorSelectionCalls;
    }
    [[nodiscard]] bool updateSelectorSelectionAt(const QPoint& physicalPoint) override {
        ++updateSelectorSelectionCalls;
        lastSelectorPoint = physicalPoint;
        selectorHitTestActive = selectorRequestStarts;
        return selectorRequestStarts;
    }
    [[nodiscard]] bool applySelectorHitPath(const QVector<QRectF>& hitRects) override {
        ++applySelectorHitPathCalls;
        lastAppliedHitPath = hitRects;
        return selectorHitPathApplies && !hitRects.isEmpty();
    }

    void prewarmDisplayPool(ScreenshotDisplaySession&, int) override {}
    void ensureToolbar() override {}
    void prewarmToolbar() override {
        ++prewarmToolbarCalls;
    }
    void clearOverlayCanvases(const ScreenshotDisplaySession&) const override {
        ++clearOverlayCanvasCalls;
    }
    void clearDisplays(ScreenshotDisplaySession&) override {
        ++clearDisplayCalls;
    }
    void destroyDisplayPool(ScreenshotDisplaySession&) override {
        ++destroyDisplayPoolCalls;
    }
    void resetForNewCapture(ScreenshotDisplaySession&) override {}
    void prepareDisplayModels(ScreenshotDisplaySession&) override {}
    void applyDisplayModels(ScreenshotDisplaySession&) override {
        ++applyDisplayModelsCalls;
    }
    [[nodiscard]] bool
    preparePreCaptureOverlayWindows(ScreenshotDisplaySession& displaySession) override {
        ++preparePreCaptureOverlayCalls;
        if (preparePreCaptureOverlaySucceeds && displaySession.isEmpty()) {
            CapturedDisplayModel display;
            display.name = QStringLiteral("Primary");
            display.physicalRect = QRect(0, 0, 64, 48);
            display.logicalRect = display.physicalRect;
            display.canvasRect = display.physicalRect;
            display.active = true;
            displaySession.appendDisplay(display);
        }
        return preparePreCaptureOverlaySucceeds;
    }
    void showOverlayWindows(const ScreenshotDisplaySession&, ScreenshotOverlayShowMode) override {
        ++showOverlayCalls;
    }
    void activateOverlayWindows(const ScreenshotDisplaySession&,
                                std::function<void()> interactionReady) override {
        ++activateOverlayCalls;
        if (interactionReady) {
            ++interactionReadyCalls;
            interactionReady();
        }
    }
    void hideOverlayWindows(const ScreenshotDisplaySession&) override {
        ++hideOverlayCalls;
    }

    [[nodiscard]] bool clearDocumentPreservingViewports() override {
        ++clearDocumentCalls;
        return true;
    }
    [[nodiscard]] bool resetCanvasRuntime() override {
        return true;
    }
    void resetColorPicker() override {}

    ScreenshotCaptureWorkerEventSink* eventSink = nullptr;
    int prepareAsyncCalls = 0;
    int prepareAsyncOrder = 0;
    int ensureCaptureWorkerCalls = 0;
    int captureAllAsyncCalls = 0;
    int cancelActiveCaptureCalls = 0;
    int releaseIdleResourcesCalls = 0;
    int releaseIdleResourcesOrder = 0;
    int lifecycleOperationOrder = 0;
    int startWorkflowRefreshCalls = 0;
    int resetHitTestStateCalls = 0;
    int clearSelectorSelectionCalls = 0;
    int updateSelectorSelectionCalls = 0;
    int applySelectorHitPathCalls = 0;
    int releaseSelectorCacheCalls = 0;
    int destroySelectorServiceCalls = 0;
    mutable int clearOverlayCanvasCalls = 0;
    int clearDisplayCalls = 0;
    int destroyDisplayPoolCalls = 0;
    int showOverlayCalls = 0;
    int activateOverlayCalls = 0;
    int interactionReadyCalls = 0;
    int applyDisplayModelsCalls = 0;
    int preparePreCaptureOverlayCalls = 0;
    int hideOverlayCalls = 0;
    int clearDocumentCalls = 0;
    int prewarmToolbarCalls = 0;
    int shutdownCaptureWorkerCalls = 0;
    bool selectorIsReady = false;
    bool selectorRefreshActive = false;
    bool selectorHitTestActive = false;
    bool selectorRequestStarts = false;
    bool selectorHitPathApplies = true;
    bool preparePreCaptureOverlaySucceeds = true;
    bool workerCreated = true;
    bool captureWasQueuedBeforeSelectorRefresh = false;
    bool failCaptureSynchronously = false;
    QPoint lastSelectorPoint;
    QVector<QRectF> lastAppliedHitPath;
    ScreenshotCaptureRequest lastCaptureRequest;
};

ScreenshotCaptureResult successfulResult(quint64 requestId,
                                         const CapturedDisplayModel& snapshot) {
    ScreenshotCaptureResult result;
    result.requestId = requestId;
    result.displays = {snapshot};
    result.succeeded = true;
    return result;
}

ScreenshotCaptureWorkflow
makeWorkflow(ScreenshotCaptureState& state, ScreenshotDisplaySession& displaySession,
             ScreenshotGeometryMapper& geometry, ScreenshotInteractionState& interaction,
             ScreenshotSelectionModel& selection,
             ScreenshotIntelligentSelectionModel& intelligentSelection, CaptureRuntime& runtime,
             bool smartSelectionEnabled = true) {
    return ScreenshotCaptureWorkflow({
        state,
        runtime,
        geometry,
        displaySession,
        interaction,
        selection,
        intelligentSelection,
        {},
        {},
        [smartSelectionEnabled]() { return smartSelectionEnabled; },
    });
}

void idlePrewarmDoesNotInitializeSelector() {
    ScreenshotCaptureState state;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.prewarmResources();
    workflow.prewarmResources();
    require(runtime.prewarmToolbarCalls == 1,
            "idle toolbar prewarm must be idempotent once resources are prepared");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared,
            "idle prewarm must leave the workflow prepared");
    require(runtime.startWorkflowRefreshCalls == 0 && !runtime.selectorRefreshActive,
            "idle prewarm must not initialize the selector cache");

    workflow.startCapture();
    workflow.prewarmResources();
    require(runtime.prewarmToolbarCalls == 1, "active capture must not run idle toolbar prewarm");
    require(runtime.startWorkflowRefreshCalls == 1 && runtime.selectorRefreshActive,
            "capture start must overlap selector initialization with native acquisition");
}

void cancelClearsTheReusableCanvasDocument() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::Editing;
    state.captureInProgress = true;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    interaction.beginCapture();
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    int captureTerminatedCalls = 0;

    ScreenshotCaptureWorkflow workflow({
        state,
        runtime,
        geometry,
        displaySession,
        interaction,
        selection,
        intelligentSelection,
        {},
        [&captureTerminatedCalls]() { ++captureTerminatedCalls; },
    });

    workflow.cancelCapture();

    require(runtime.clearDocumentCalls == 1,
            "canceling a capture must clear the reusable canvas document");
    require(runtime.clearOverlayCanvasCalls == 1,
            "clearing the canceled document must refresh reused overlay canvases");
    require(runtime.hideOverlayCalls == 1 && runtime.destroyDisplayPoolCalls == 1,
            "canceling a capture must destroy its full-screen overlay pool");
    require(runtime.destroySelectorServiceCalls == 1 && runtime.releaseSelectorCacheCalls == 0,
            "canceling a capture must destroy the selector service instead of retaining it");
    require(runtime.cancelActiveCaptureCalls == 1,
            "canceling a capture must signal the native cancellation token");
    require(runtime.shutdownCaptureWorkerCalls == 1 && runtime.releaseIdleResourcesCalls == 0 &&
                runtime.prepareAsyncCalls == 0,
            "canceling a capture must destroy native capture resources without re-prewarming");
    require(captureTerminatedCalls == 1,
            "canceling a capture must stop active capture-scoped features before cleanup");
    require(state.sessionState == ScreenshotSessionState::IdleCold,
            "canceling a capture must return the workflow to its cold idle state");

    workflow.startCapture();
    require(runtime.ensureCaptureWorkerCalls == 1 && runtime.captureAllAsyncCalls == 1 &&
                state.sessionState == ScreenshotSessionState::Capturing && state.captureInProgress,
            "the next capture must recreate cold-released native resources on demand");
    require(runtime.prepareAsyncCalls == 0 && runtime.prewarmToolbarCalls == 0,
            "the next capture must not depend on idle prewarm after cold cancellation");
}

void captureStartsSelectorInitializationWhilePixelsAreInFlight() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;

    auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();

    require(runtime.captureAllAsyncCalls == 1 &&
                runtime.captureWasQueuedBeforeSelectorRefresh &&
                runtime.startWorkflowRefreshCalls == 1 && runtime.selectorRefreshActive,
            "capture start must queue acquisition first and then overlap selector refresh");

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);

    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));

    require(runtime.startWorkflowRefreshCalls == 1 && runtime.selectorRefreshActive,
            "capture completion must not redundantly restart the selector snapshot");
}

void presentationWaitsForPixelsAndInitialSelectionInEitherOrder() {
    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);

    {
        ScreenshotCaptureState state;
        state.sessionState = ScreenshotSessionState::IdlePrepared;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligentSelection;
        CaptureRuntime runtime;
        runtime.selectorRequestStarts = true;
        int initialFramePrepared = 0;
        ScreenshotCaptureWorkflow workflow({
            state,
            runtime,
            geometry,
            displays,
            interaction,
            selection,
            intelligentSelection,
            ScreenshotCapturePresentationCallbacks{{}, {}, {}, {}, {},
                                                   [&initialFramePrepared]() {
                                                       ++initialFramePrepared;
                                                   }},
        });

        workflow.startCapture();
        runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));
        require(runtime.showOverlayCalls == 0 && initialFramePrepared == 0,
                "pixels-first capture must remain hidden until initial selection resolves");

        const QVector<QRectF> path{QRectF(0, 0, 64, 48)};
        require(workflow.handleInitialSmartSelectionResult(state.sessionId,
                                                           runtime.lastSelectorPoint, true, path),
                "active initial selector result must be consumed by capture workflow");
        require(runtime.applySelectorHitPathCalls == 1 &&
                    runtime.lastAppliedHitPath == path && initialFramePrepared == 1 &&
                    runtime.showOverlayCalls == 1,
                "pixels-first capture must reveal one complete selected frame");
        require(!workflow.handleInitialSmartSelectionResult(
                    state.sessionId, runtime.lastSelectorPoint, true, path),
                "post-presentation hover results must return to the normal selector workflow");
    }

    {
        ScreenshotCaptureState state;
        state.sessionState = ScreenshotSessionState::IdlePrepared;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligentSelection;
        CaptureRuntime runtime;
        runtime.selectorRequestStarts = true;
        auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                     intelligentSelection, runtime);

        workflow.startCapture();
        const QVector<QRectF> path{QRectF(0, 0, 64, 48)};
        require(workflow.handleInitialSmartSelectionResult(state.sessionId,
                                                           runtime.lastSelectorPoint, true, path),
                "selection-first result must be retained for the active capture");
        require(runtime.showOverlayCalls == 0 && runtime.applySelectorHitPathCalls == 0,
                "selection-first capture must remain hidden until pixels arrive");

        runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));
        require(runtime.applySelectorHitPathCalls == 1 && runtime.showOverlayCalls == 1,
                "selection-first capture must reveal one complete selected frame");
    }
}

void initialSelectorFailureResolvesToManualSelection() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.selectorRequestStarts = true;
    auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();
    require(workflow.handleInitialSmartSelectionResult(state.sessionId,
                                                       runtime.lastSelectorPoint, false, {}),
            "failed initial result must still resolve the presentation barrier");

    CapturedDisplayModel snapshot;
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));

    require(runtime.showOverlayCalls == 1 && runtime.applySelectorHitPathCalls == 0 &&
                interaction.manualSelecting(),
            "selector failure must show once in manual mode instead of deadlocking");
}

void initialSelectionCoalescesToTheLatestCursorPoint() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.selectorRequestStarts = true;
    auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();
    const QPoint stalePoint = runtime.lastSelectorPoint + QPoint(1, 0);
    require(workflow.handleInitialSmartSelectionResult(
                state.sessionId, stalePoint, true, {QRectF(0, 0, 64, 48)}),
            "a superseded initial result must be consumed");
    require(runtime.updateSelectorSelectionCalls == 2 && runtime.showOverlayCalls == 0,
            "a superseded result must queue the latest point without resolving presentation");

    const QPoint latestPoint = runtime.lastSelectorPoint;
    require(workflow.handleInitialSmartSelectionResult(
                state.sessionId, latestPoint, true, {QRectF(0, 0, 64, 48)}),
            "the latest initial result must be consumed");

    CapturedDisplayModel snapshot;
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));
    require(runtime.showOverlayCalls == 1,
            "the latest settled cursor result must unlock the complete first frame");
}

void changedCaptureGeometryInvalidatesTheEarlySelectorResult() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.selectorRequestStarts = true;
    auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();
    require(workflow.handleInitialSmartSelectionResult(
                state.sessionId, runtime.lastSelectorPoint, true, {QRectF(0, 0, 64, 48)}),
            "early selector result must be retained until capture geometry is known");

    CapturedDisplayModel changedSnapshot;
    changedSnapshot.name = QStringLiteral("Primary");
    changedSnapshot.physicalRect = QRect(0, 0, 80, 60);
    changedSnapshot.logicalRect = changedSnapshot.physicalRect;
    changedSnapshot.image =
        QImage(changedSnapshot.physicalRect.size(), QImage::Format_RGBA8888);
    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, changedSnapshot));
    require(runtime.resetHitTestStateCalls == 1 && runtime.startWorkflowRefreshCalls == 2 &&
                runtime.showOverlayCalls == 0,
            "changed final geometry must invalidate the early result and refresh the selector");

    require(workflow.handleInitialSmartSelectionResult(
                state.sessionId, runtime.lastSelectorPoint, true, {QRectF(0, 0, 80, 60)}),
            "geometry retry result must be consumed");
    require(runtime.showOverlayCalls == 1 && runtime.applySelectorHitPathCalls == 1,
            "geometry retry must reveal exactly one remapped complete frame");
}

void changedCaptureGeometryInvalidatesAnInFlightSelectorRequest() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.selectorRequestStarts = true;
    auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();

    CapturedDisplayModel changedSnapshot;
    changedSnapshot.name = QStringLiteral("Primary");
    changedSnapshot.physicalRect = QRect(0, 0, 80, 60);
    changedSnapshot.logicalRect = changedSnapshot.physicalRect;
    changedSnapshot.image =
        QImage(changedSnapshot.physicalRect.size(), QImage::Format_RGBA8888);
    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, changedSnapshot));

    require(runtime.resetHitTestStateCalls == 1 && runtime.startWorkflowRefreshCalls == 2 &&
                runtime.updateSelectorSelectionCalls == 2 && runtime.showOverlayCalls == 0,
            "changed final geometry must replace an in-flight early selector request");

    require(workflow.handleInitialSmartSelectionResult(
                state.sessionId, runtime.lastSelectorPoint, true, {QRectF(0, 0, 80, 60)}),
            "replacement selector result must be consumed");
    require(runtime.showOverlayCalls == 1 && runtime.applySelectorHitPathCalls == 1,
            "replacement result must reveal exactly one complete frame");
}

void canceledCaptureRejectsItsStaleSelectorResult() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.selectorRequestStarts = true;
    auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();
    const quint64 canceledSession = state.sessionId;
    const QPoint requestPoint = runtime.lastSelectorPoint;
    workflow.cancelCapture();

    require(!workflow.handleInitialSmartSelectionResult(
                canceledSession, requestPoint, true, {QRectF(0, 0, 64, 48)}) &&
                runtime.showOverlayCalls == 0,
            "a canceled session must reject stale selector completion");
}

void canceledCaptureIgnoresStalePixelsWithoutInitializingSelector() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;

    auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture();
    const quint64 canceledSessionId = state.sessionId;
    workflow.cancelCapture();

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);

    runtime.eventSink->handleCaptureFinished(successfulResult(canceledSessionId, snapshot));

    require(runtime.startWorkflowRefreshCalls == 1 && runtime.applyDisplayModelsCalls == 0 &&
                runtime.showOverlayCalls == 0,
            "stale pixels must not restart selector work or initialize presentation resources");
}

void synchronousCaptureFailureDoesNotRestartSelectorRefresh() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.failCaptureSynchronously = true;

    auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                 intelligentSelection, runtime);
    workflow.startCapture();

    require(!state.captureInProgress && state.sessionState == ScreenshotSessionState::IdleCold,
            "synchronous capture failure must return the workflow to idle");
    require(runtime.startWorkflowRefreshCalls == 0 && !runtime.selectorRefreshActive,
            "synchronous capture failure must not restart selector refresh after cleanup");
    require(runtime.shutdownCaptureWorkerCalls == 1 && runtime.destroySelectorServiceCalls == 1 &&
                runtime.destroyDisplayPoolCalls == 1 && runtime.prepareAsyncCalls == 0,
            "synchronous failure cleanup must cold-release capture resources");
}

void restartingCaptureReleasesPreviousSelectorCache() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::Editing;
    state.captureInProgress = true;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    interaction.beginCapture();
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.selectorIsReady = true;
    int captureTerminatedCalls = 0;
    ScreenshotCaptureWorkflow workflow({
        state,
        runtime,
        geometry,
        displaySession,
        interaction,
        selection,
        intelligentSelection,
        {},
        [&captureTerminatedCalls]() { ++captureTerminatedCalls; },
    });

    workflow.startCapture();

    require(runtime.releaseSelectorCacheCalls == 1,
            "starting a new capture must release the previous selector cache");
    require(runtime.startWorkflowRefreshCalls == 1,
            "the restarted capture must overlap its fresh selector snapshot with acquisition");
    require(captureTerminatedCalls == 1,
            "restarting a capture must stop features owned by the previous capture");
}

void capturePresentedRunsAfterCapturedOverlayIsShown() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    int capturePresentedCalls = 0;
    int showCallsObservedByCallback = 0;
    int activationCallsObservedByCallback = 0;
    ScreenshotCaptureWorkflow workflow({
        state,
        runtime,
        geometry,
        displaySession,
        interaction,
        selection,
        intelligentSelection,
        ScreenshotCapturePresentationCallbacks{
            {},
            {},
            {},
            [&capturePresentedCalls, &showCallsObservedByCallback,
             &activationCallsObservedByCallback, &runtime]() {
                ++capturePresentedCalls;
                showCallsObservedByCallback = runtime.showOverlayCalls;
                activationCallsObservedByCallback = runtime.activateOverlayCalls;
            },
        },
    });

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    snapshot.image.fill(Qt::blue);

    workflow.startCapture();
    require(runtime.eventSink != nullptr, "capture workflow did not register its event sink");
    const ScreenshotCaptureResult result = successfulResult(state.sessionId, snapshot);
    runtime.eventSink->handleCaptureFinished(result);
    runtime.eventSink->handleCaptureFinished(result);

    require(runtime.showOverlayCalls == 1 && runtime.activateOverlayCalls == 1 &&
                runtime.interactionReadyCalls == 1 && capturePresentedCalls == 1 &&
                showCallsObservedByCallback == 1 &&
                activationCallsObservedByCallback == 0,
            "capture presentation must be reported after show and before input activation");
}

void silentCaptureNeverPreparesOrShowsOverlays() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    int capturePresentedCalls = 0;
    ScreenshotCaptureWorkflow workflow({
        state,
        runtime,
        geometry,
        displaySession,
        interaction,
        selection,
        intelligentSelection,
        ScreenshotCapturePresentationCallbacks{
            {},
            {},
            {},
            [&capturePresentedCalls]() { ++capturePresentedCalls; },
        },
    });

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    snapshot.image.fill(Qt::blue);

    workflow.startCapture(ScreenshotCapturePresentationMode::Silent);
    require(runtime.eventSink != nullptr, "capture workflow did not register its event sink");
    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));

    require(runtime.preparePreCaptureOverlayCalls == 0,
            "silent capture must not prepare screenshot windows");
    require(runtime.showOverlayCalls == 0 && runtime.applyDisplayModelsCalls == 0,
            "silent capture must not bind or show screenshot windows");
    require(runtime.startWorkflowRefreshCalls == 0,
            "silent capture must not initialize smart selection");
    require(capturePresentedCalls == 1,
            "silent capture must notify the controller when pixels are ready");
}

void focusedWindowCaptureUsesOneCompoundWorkerTransaction() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    std::optional<ScreenshotWindowCaptureFrame> deliveredFocusedWindow;
    ScreenshotCaptureWorkflow workflow({
        state,
        runtime,
        geometry,
        displaySession,
        interaction,
        selection,
        intelligentSelection,
        {},
        {},
        []() { return true; },
        [&deliveredFocusedWindow](std::optional<ScreenshotWindowCaptureFrame> frame) {
            deliveredFocusedWindow = std::move(frame);
        },
    });

    constexpr quintptr focusedHandle = 0x1234;
    workflow.startCapture(ScreenshotCapturePresentationMode::Silent, focusedHandle);
    require(runtime.captureAllAsyncCalls == 1 &&
                runtime.lastCaptureRequest.requestId == state.sessionId &&
                runtime.lastCaptureRequest.focusedWindowHandle == focusedHandle,
            "focused capture must submit the HWND with the desktop request");

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    snapshot.image.fill(Qt::blue);

    ScreenshotCaptureResult result = successfulResult(state.sessionId, snapshot);
    ScreenshotWindowCaptureFrame focused;
    focused.physicalRect = QRect(8, 9, 20, 12);
    focused.image = QImage(focused.physicalRect.size(), QImage::Format_RGBA8888);
    focused.image.fill(Qt::red);
    result.focusedWindow = focused;
    runtime.eventSink->handleCaptureFinished(result);

    require(deliveredFocusedWindow.has_value() && deliveredFocusedWindow->isValid(),
            "focused frame must be delivered before the capture is presented");
}

void focusedWindowCaptureIsAllOrNothing() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                 intelligentSelection, runtime);

    workflow.startCapture(ScreenshotCapturePresentationMode::Silent, 0x4321);
    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    ScreenshotCaptureResult result = successfulResult(state.sessionId, snapshot);
    runtime.eventSink->handleCaptureFinished(result);

    require(!state.captureInProgress && state.sessionState == ScreenshotSessionState::IdleCold &&
                runtime.cancelActiveCaptureCalls == 1,
            "missing focused pixels must cancel the compound request");
}

void capturedImagePlacementFollowsNormalizedCanvasGeometry() {
    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("secondary-display");
    snapshot.name = QStringLiteral("Secondary");
    snapshot.physicalRect = QRect(1920, 240, 320, 180);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    snapshot.image.fill(Qt::red);

    ScreenshotDisplaySession displaySession;
    ScreenshotCaptureDisplayModelReconciler::applySnapshots(displaySession, {snapshot});

    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displaySession);

    const CapturedDisplayModel& display = displaySession.displayAt(0);
    require(ScreenshotGeometryMapper::displayCanvasRect(display) == QRectF(0, 0, 320, 180),
            "capture geometry must normalize a non-zero physical monitor origin");
    require(ScreenshotGeometryMapper::displayImageSourceCanvasRect(display) ==
                ScreenshotGeometryMapper::displayCanvasRect(display),
            "captured image placement must follow normalized canvas geometry");
}

void intelligentSelectionTargetsPreserveElementPathBehavior() {
    ScreenshotIntelligentSelectionModel selection;
    const QRectF nestedElement(30, 30, 20, 10);
    const QRectF childElement(20, 20, 60, 40);
    const QRectF window(10, 10, 100, 80);
    const QRectF bounds(0, 0, 200, 160);

    selection.beginCaptureSession(true);
    require(selection.smartSelectionEnabled() &&
                selection.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                selection.applyCanvasHitPath({nestedElement, childElement, window}, bounds, 1.0) &&
                selection.index() == 0 && selection.currentSelection() == nestedElement,
            "enabled Smart Selection must initially capture the deepest child element");
    require(selection.setIndex(1) && selection.currentSelection() == childElement &&
                selection.applyCanvasHitPath({nestedElement, childElement, window}, bounds, 1.0) &&
                selection.index() == 1,
            "an unchanged element hit path must preserve its selected level");

    const QRectF nextElement(130, 30, 20, 10);
    const QRectF nextWindow(120, 10, 70, 80);
    require(selection.applyCanvasHitPath({nextElement, nextWindow}, bounds, 1.0) &&
                selection.index() == 0 && selection.currentSelection() == nextElement,
            "a changed element hit path must restart at its deepest hit");
    require(selection.setIndex(99) && selection.index() == 1 &&
                selection.currentSelection() == nextWindow,
            "element selection must retain the original full-path navigation");
    require(selection.applyCanvasHitPath({nextWindow}, bounds, 1.0) && selection.index() == 0 &&
                selection.currentSelection() == nextWindow,
            "element selection must retain the original window fallback");

    require(selection.toggleSelectionTarget() &&
                selection.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                selection.currentSelection() == nextWindow && selection.setIndex(0) &&
                selection.index() == 0,
            "window mode must lock an element path to its outermost window");

    selection.beginCaptureSession(true);
    require(selection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "a new screenshot must discard the preceding screenshot's Tab target mode");

    selection.beginCaptureSession(false);
    require(!selection.smartSelectionEnabled() &&
                selection.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                !selection.toggleSelectionTarget() &&
                selection.applyCanvasHitPath({nestedElement, childElement, window}, bounds, 1.0) &&
                selection.index() == 2 && selection.currentSelection() == window,
            "disabled Smart Selection must remain locked to window capture");

    require(selection.updateSmartSelectionEnabled(true) &&
                selection.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                selection.index() == 0 && selection.currentSelection() == nestedElement &&
                selection.updateSmartSelectionEnabled(false) &&
                selection.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                selection.index() == 2 && selection.currentSelection() == window,
            "a live Smart Selection setting change must immediately enforce its target policy");
}

void captureSessionsApplyTheCurrentSmartSelectionSetting() {
    ScreenshotCaptureState enabledState;
    enabledState.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession enabledDisplays;
    ScreenshotGeometryMapper enabledGeometry;
    ScreenshotInteractionState enabledInteraction;
    ScreenshotSelectionModel enabledSelection;
    ScreenshotIntelligentSelectionModel enabledIntelligentSelection;
    CaptureRuntime enabledRuntime;
    auto enabledWorkflow = makeWorkflow(
        enabledState, enabledDisplays, enabledGeometry, enabledInteraction, enabledSelection,
        enabledIntelligentSelection, enabledRuntime, true);

    enabledWorkflow.startCapture();
    require(enabledIntelligentSelection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "an enabled capture session must begin in child-element mode");
    require(enabledIntelligentSelection.toggleSelectionTarget(),
            "enabled capture session must allow the target mode to switch");
    enabledWorkflow.startCapture();
    require(enabledIntelligentSelection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "restarting capture must restore the enabled session's initial child-element mode");

    ScreenshotCaptureState disabledState;
    disabledState.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession disabledDisplays;
    ScreenshotGeometryMapper disabledGeometry;
    ScreenshotInteractionState disabledInteraction;
    ScreenshotSelectionModel disabledSelection;
    ScreenshotIntelligentSelectionModel disabledIntelligentSelection;
    CaptureRuntime disabledRuntime;
    auto disabledWorkflow = makeWorkflow(
        disabledState, disabledDisplays, disabledGeometry, disabledInteraction, disabledSelection,
        disabledIntelligentSelection, disabledRuntime, false);

    disabledWorkflow.startCapture();
    require(disabledIntelligentSelection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::Window &&
                !disabledIntelligentSelection.toggleSelectionTarget(),
            "a disabled capture session must stay in window mode");
}
} // namespace

int main() {
    idlePrewarmDoesNotInitializeSelector();
    cancelClearsTheReusableCanvasDocument();
    captureStartsSelectorInitializationWhilePixelsAreInFlight();
    presentationWaitsForPixelsAndInitialSelectionInEitherOrder();
    initialSelectorFailureResolvesToManualSelection();
    initialSelectionCoalescesToTheLatestCursorPoint();
    changedCaptureGeometryInvalidatesTheEarlySelectorResult();
    changedCaptureGeometryInvalidatesAnInFlightSelectorRequest();
    canceledCaptureRejectsItsStaleSelectorResult();
    canceledCaptureIgnoresStalePixelsWithoutInitializingSelector();
    synchronousCaptureFailureDoesNotRestartSelectorRefresh();
    restartingCaptureReleasesPreviousSelectorCache();
    capturePresentedRunsAfterCapturedOverlayIsShown();
    silentCaptureNeverPreparesOrShowsOverlays();
    focusedWindowCaptureUsesOneCompoundWorkerTransaction();
    focusedWindowCaptureIsAllOrNothing();
    capturedImagePlacementFollowsNormalizedCanvasGeometry();
    intelligentSelectionTargetsPreserveElementPathBehavior();
    captureSessionsApplyTheCurrentSmartSelectionSetting();
    return 0;
}
