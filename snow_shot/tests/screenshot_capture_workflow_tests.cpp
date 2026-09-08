#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h"
#include "snow_shot/presentation/screenshotcaptureworkflow.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"

#include <QVector>

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
        return true;
    }
    void ensureCaptureWorker() override {}
    void prepareAsync(quint64) override {
        ++prepareAsyncCalls;
    }
    void refreshLayoutAsync(quint64 requestId) override {
        ++refreshLayoutCalls;
        lastRefreshRequestId = requestId;
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
    void shutdownCaptureWorker() override {}

    [[nodiscard]] bool selectorReady() const override {
        return selectorIsReady;
    }
    [[nodiscard]] bool selectorRefreshInFlight() const override {
        return selectorRefreshActive;
    }
    [[nodiscard]] bool selectorHitTestInFlight() const override {
        return false;
    }
    void releaseSelectorCache() override {
        ++releaseSelectorCacheCalls;
        selectorIsReady = false;
        selectorRefreshActive = false;
    }
    void resetHitTestState() override {}
    void destroySelectorService() override {}
    void startWorkflowRefresh() override {
        ++startWorkflowRefreshCalls;
        selectorIsReady = false;
        selectorRefreshActive = true;
    }
    void clearSelectorSelection() override {}
    [[nodiscard]] bool updateSelectorSelectionAt(const QPoint&) override {
        return acceptSelectorHitTest;
    }

    void prewarmDisplayPool(ScreenshotDisplaySession&, int) override {
        ++prewarmDisplayPoolCalls;
        prewarmDisplayPoolSawRuntimeReset = resetForNewCaptureCalls > 0;
    }
    void clearOverlayCanvases(const ScreenshotDisplaySession&) const override {
        ++clearOverlayCanvasCalls;
    }
    void clearDisplays(ScreenshotDisplaySession&) override {
        ++clearDisplayCalls;
    }
    void destroyDisplayPool(ScreenshotDisplaySession&) override {}
    void resetForNewCapture(ScreenshotDisplaySession&) override {
        ++resetForNewCaptureCalls;
    }
    void prepareDisplayModels(ScreenshotDisplaySession&) override {}
    void applyDisplayModels(ScreenshotDisplaySession&) override {
        ++applyDisplayModelsCalls;
    }
    [[nodiscard]] bool
    preparePreCaptureOverlayWindows(ScreenshotDisplaySession& displaySession) override {
        ++preparePreCaptureOverlayCalls;
        if (seedActiveDisplayOnPrepare) {
            CapturedDisplayModel display;
            display.stableId = QStringLiteral("primary");
            display.name = QStringLiteral("Primary");
            display.physicalRect = QRect(0, 0, 64, 48);
            display.logicalRect = display.physicalRect;
            display.active = true;
            displaySession.appendDisplay(display);
        }
        return true;
    }
    void showOverlayWindows(const ScreenshotDisplaySession&,
                            ScreenshotOverlayShowMode mode) override {
        ++showOverlayCalls;
        showOverlayModes.push_back(mode);
        if (mode == ScreenshotOverlayShowMode::WarmSurface) {
            ++warmSurfaceShowCalls;
        }
        if (mode == ScreenshotOverlayShowMode::CapturedImage) {
            ++capturedImageShowCalls;
        }
    }
    void hideOverlayWindowsImmediately(const ScreenshotDisplaySession&) override {
        ++hideOverlayImmediatelyCalls;
        operations.push_back(QStringLiteral("hide-overlays-immediately"));
    }
    void hideOverlayWindows(const ScreenshotDisplaySession&) override {
        ++hideOverlayCalls;
    }
    void prewarmToolbarSurface(const ScreenshotDisplaySession&) override {
        ++prewarmToolbarSurfaceCalls;
        prewarmToolbarSawDispatchedCapture = captureAllAsyncCalls > 0;
    }

    [[nodiscard]] bool clearDocumentPreservingViewports() override {
        ++clearDocumentCalls;
        operations.push_back(QStringLiteral("clear-document"));
        return true;
    }
    [[nodiscard]] bool resetCanvasRuntime() override {
        return true;
    }
    void resetColorPicker() override {}

    ScreenshotCaptureWorkerEventSink* eventSink = nullptr;
    int prepareAsyncCalls = 0;
    int captureAllAsyncCalls = 0;
    int cancelActiveCaptureCalls = 0;
    int refreshLayoutCalls = 0;
    quint64 lastRefreshRequestId = 0;
    int startWorkflowRefreshCalls = 0;
    int releaseSelectorCacheCalls = 0;
    mutable int clearOverlayCanvasCalls = 0;
    int clearDisplayCalls = 0;
    int showOverlayCalls = 0;
    int warmSurfaceShowCalls = 0;
    int capturedImageShowCalls = 0;
    QVector<ScreenshotOverlayShowMode> showOverlayModes;
    int applyDisplayModelsCalls = 0;
    int preparePreCaptureOverlayCalls = 0;
    int hideOverlayCalls = 0;
    int hideOverlayImmediatelyCalls = 0;
    int prewarmToolbarSurfaceCalls = 0;
    bool prewarmToolbarSawDispatchedCapture = false;
    int resetForNewCaptureCalls = 0;
    int clearDocumentCalls = 0;
    int prewarmDisplayPoolCalls = 0;
    bool prewarmDisplayPoolSawRuntimeReset = false;
    bool selectorIsReady = false;
    bool selectorRefreshActive = false;
    bool acceptSelectorHitTest = false;
    bool captureWasQueuedBeforeSelectorRefresh = false;
    bool failCaptureSynchronously = false;
    bool seedActiveDisplayOnPrepare = false;
    ScreenshotCaptureRequest lastCaptureRequest;
    QVector<QString> operations;
};

ScreenshotCaptureResult successfulResult(quint64 requestId, const CapturedDisplayModel& snapshot) {
    ScreenshotCaptureResult result;
    result.requestId = requestId;
    result.displays = {snapshot};
    result.succeeded = true;
    return result;
}

ScreenshotCaptureWorkflow makeWorkflow(ScreenshotCaptureState& state,
                                       ScreenshotDisplaySession& displaySession,
                                       ScreenshotGeometryMapper& geometry,
                                       ScreenshotInteractionState& interaction,
                                       ScreenshotSelectionModel& selection,
                                       ScreenshotIntelligentSelectionModel& intelligentSelection,
                                       CaptureRuntime& runtime, bool smartSelectionEnabled = true) {
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

void captureRestoresSelectionEffectsAfterReset() {
    for (const bool prewarm : {false, true}) {
        ScreenshotCaptureState state;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligentSelection;
        CaptureRuntime runtime;
        ScreenshotCaptureWorkflowContext context{
            state, runtime, geometry, displays, interaction, selection, intelligentSelection, {}};
        int radius = 24;
        int shadowWidth = 12;
        context.restoreSelectionEffects = [&]() {
            static_cast<void>(selection.setCornerRadius(radius));
            static_cast<void>(selection.setShadowWidth(shadowWidth));
        };
        ScreenshotCaptureWorkflow workflow(std::move(context));
        if (prewarm) {
            workflow.prewarmResources();
        }
        workflow.startCapture();
        require(selection.cornerRadius() == 24 && selection.shadowWidth() == 12,
                "cold and prewarmed captures must restore effects after resetting the model");
        require(!selection.hasPixelSelection() && !selection.aspectRatioLocked(),
                "restoring effects must not restore selection geometry or aspect ratio locking");
        workflow.cancelCapture();
        radius = 32;
        shadowWidth = 16;
        workflow.startCapture();
        require(selection.cornerRadius() == 32 && selection.shadowWidth() == 16,
                "captures after cancellation must reload the latest saved effects");
        radius = 0;
        shadowWidth = 0;
        workflow.startCapture();
        require(selection.cornerRadius() == 0 && selection.shadowWidth() == 0,
                "restarting an active capture must restore disabled effects");
    }
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
    require(runtime.prepareAsyncCalls == 1 && runtime.prewarmDisplayPoolCalls == 1,
            "idle kernel preparation must be idempotent once resources are prepared");
    require(runtime.prewarmDisplayPoolSawRuntimeReset,
            "idle preparation must prewarm overlay surfaces after runtime cleanup");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared,
            "idle prewarm must leave the workflow prepared");
    require(runtime.startWorkflowRefreshCalls == 0 && !runtime.selectorRefreshActive,
            "idle prewarm must not initialize the selector cache");
    require(runtime.prewarmToolbarSurfaceCalls == 0,
            "idle prewarm must not create the editing toolbar surface");

    workflow.startCapture();
    workflow.prewarmResources();
    require(runtime.startWorkflowRefreshCalls == 1 && runtime.selectorRefreshActive,
            "capture start must initialize the selector cache");
}

void endingScreenshotReprewarmsOverlaySurfaces() {
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
    require(runtime.hideOverlayCalls == 1 && runtime.clearDisplayCalls == 1,
            "canceling a capture must still release its visible display session");
    require(runtime.releaseSelectorCacheCalls == 1,
            "canceling a capture must immediately release the selector cache");
    require(runtime.cancelActiveCaptureCalls == 1,
            "canceling a capture must signal the native cancellation token");
    require(runtime.prepareAsyncCalls == 0,
            "ending a screenshot must not prepare the native capture worker again");
    require(runtime.prewarmDisplayPoolCalls == 1 && runtime.prewarmDisplayPoolSawRuntimeReset,
            "ending a screenshot must re-prewarm overlay surfaces after runtime cleanup");
    require(captureTerminatedCalls == 1,
            "canceling a capture must stop active capture-scoped features before cleanup");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared,
            "canceling a capture must return the workflow to its prepared idle state");
}

void cancelConcealsOverlayBeforeClearingVisibleFrame() {
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

    auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                 intelligentSelection, runtime);
    workflow.cancelCapture();

    const qsizetype concealIndex =
        runtime.operations.indexOf(QStringLiteral("hide-overlays-immediately"));
    const qsizetype clearIndex = runtime.operations.indexOf(QStringLiteral("clear-document"));
    require(concealIndex >= 0 && clearIndex >= 0 && concealIndex < clearIndex,
            "canceling a capture must conceal the overlay before clearing its visible frame");
}

void exportCancellationDefersExpensiveCleanup() {
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

    workflow.cancelCaptureForExport();
    require(runtime.cancelActiveCaptureCalls == 1 && captureTerminatedCalls == 1,
            "export cancellation must stop the active capture before presenting the pin");
    require(runtime.hideOverlayImmediatelyCalls == 1 && runtime.hideOverlayCalls == 0,
            "export cancellation must use the immediate overlay hide path");
    require(runtime.resetForNewCaptureCalls == 0,
            "export cancellation must defer the expensive capture reset");
    require(runtime.prewarmDisplayPoolCalls == 0,
            "export cancellation must defer overlay surface prewarming with cleanup");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared && !state.captureInProgress,
            "export cancellation must leave the workflow ready for presentation");

    workflow.completeDeferredExportCleanup();
    require(runtime.resetForNewCaptureCalls == 1,
            "deferred export cleanup must perform the capture reset later");
    require(runtime.prewarmDisplayPoolCalls == 1 && runtime.prewarmDisplayPoolSawRuntimeReset,
            "deferred export cleanup must re-prewarm overlay surfaces after runtime cleanup");
    workflow.completeDeferredExportCleanup();
    require(runtime.resetForNewCaptureCalls == 1 && runtime.prewarmDisplayPoolCalls == 1,
            "deferred export cleanup must be idempotent");
}

void captureOverlapsSelectorInitialization() {
    const auto runScenario = [](bool selectorReady, bool selectorRefreshActive) {
        ScreenshotCaptureState state;
        state.sessionState = ScreenshotSessionState::IdlePrepared;
        ScreenshotDisplaySession displaySession;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligentSelection;
        CaptureRuntime runtime;
        runtime.selectorIsReady = selectorReady;
        runtime.selectorRefreshActive = selectorRefreshActive;

        auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                     intelligentSelection, runtime);

        workflow.startCapture();

        require(runtime.startWorkflowRefreshCalls == 1,
                "capture must initialize the selector snapshot");
        require(runtime.captureAllAsyncCalls == 1 &&
                    runtime.captureWasQueuedBeforeSelectorRefresh && runtime.selectorRefreshActive,
                "desktop capture must overlap selector initialization after overlay exclusion");
    };

    runScenario(false, false);
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

    require(!state.captureInProgress && state.sessionState == ScreenshotSessionState::IdlePrepared,
            "synchronous capture failure must return the workflow to idle");
    require(runtime.startWorkflowRefreshCalls == 0 && !runtime.selectorRefreshActive,
            "synchronous capture failure must not restart selector refresh after cleanup");
    require(runtime.prepareAsyncCalls == 0,
            "synchronous failure cleanup must not prepare the native capture worker again");
    require(runtime.prewarmDisplayPoolCalls == 1 && runtime.prewarmDisplayPoolSawRuntimeReset,
            "synchronous failure cleanup must restore the prepared overlay surface state");
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
            "the restarted capture must initialize a fresh selector snapshot");
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
            [&capturePresentedCalls, &showCallsObservedByCallback, &runtime]() {
                ++capturePresentedCalls;
                showCallsObservedByCallback = runtime.showOverlayCalls;
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

    require(runtime.showOverlayCalls == 1 && runtime.capturedImageShowCalls == 1 &&
                runtime.warmSurfaceShowCalls == 0 && capturePresentedCalls == 1 &&
                showCallsObservedByCallback == 1,
            "capture-presented callback must run once after the captured overlay is shown");
}

void capturedOverlayWaitsForImageAndSelectionInEitherOrder() {
    for (bool selectionFirst : {false, true}) {
        ScreenshotCaptureState state;
        state.sessionState = ScreenshotSessionState::IdlePrepared;
        ScreenshotDisplaySession displaySession;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligentSelection;
        CaptureRuntime runtime;
        runtime.seedActiveDisplayOnPrepare = true;
        runtime.acceptSelectorHitTest = true;
        auto workflow = makeWorkflow(state, displaySession, geometry, interaction, selection,
                                     intelligentSelection, runtime);

        workflow.startCapture();
        require(runtime.showOverlayCalls == 0,
                "capture preparation must not show or warm overlays during initial selection");
        CapturedDisplayModel snapshot;
        snapshot.stableId = QStringLiteral("primary");
        snapshot.physicalRect = QRect(0, 0, 64, 48);
        snapshot.logicalRect = snapshot.physicalRect;
        snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGB32);
        snapshot.image.fill(Qt::blue);
        const ScreenshotCaptureResult result = successfulResult(state.sessionId, snapshot);

        if (selectionFirst) {
            workflow.handleInitialSmartSelectionResolved(state.sessionId);
        } else {
            runtime.eventSink->handleCaptureFinished(result);
        }
        require(runtime.showOverlayCalls == 0,
                "neither the image nor the selection alone may show or warm the overlay");
        workflow.handleInitialSmartSelectionResolved(state.sessionId + 1);
        require(runtime.showOverlayCalls == 0,
                "a different session's selection must not release the reveal gate");

        if (selectionFirst) {
            runtime.eventSink->handleCaptureFinished(result);
        } else {
            workflow.handleInitialSmartSelectionResolved(state.sessionId);
        }
        require(runtime.showOverlayCalls == 1 && runtime.capturedImageShowCalls == 1 &&
                    runtime.applyDisplayModelsCalls == 1,
                "image and selection readiness must reveal the prepared overlay once");
        workflow.handleInitialSmartSelectionResolved(state.sessionId);
        runtime.eventSink->handleCaptureFinished(result);
        require(runtime.showOverlayCalls == 1 && runtime.capturedImageShowCalls == 1,
                "duplicate readiness callbacks must not reveal or repaint the frame again");
    }
}

void overlayCapturePrewarmsToolbarSurfaceAfterCaptureDispatch() {
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
    require(runtime.prewarmToolbarSurfaceCalls == 1 && runtime.prewarmToolbarSawDispatchedCapture,
            "an overlay capture must prewarm the hidden toolbar surface after the capture is "
            "dispatched");

    workflow.startCapture();
    require(runtime.prewarmToolbarSurfaceCalls == 2,
            "every capture session must prewarm the toolbar surface again once the previous "
            "session retired it");
}

void overlayCaptureDoesNotWarmNativeSurfaceAfterCaptureDispatch() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displaySession;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.seedActiveDisplayOnPrepare = true;
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

    workflow.startCapture();
    require(runtime.captureAllAsyncCalls == 1 && runtime.showOverlayCalls == 0 &&
                runtime.showOverlayModes.isEmpty(),
            "an overlay capture must not warm the native surface after capture is dispatched");
    require(runtime.prewarmToolbarSawDispatchedCapture,
            "hidden toolbar preparation must still overlap the dispatched capture");

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGB32);
    snapshot.image.fill(Qt::blue);

    require(runtime.eventSink != nullptr, "capture workflow did not register its event sink");
    runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));

    require(runtime.capturedImageShowCalls == 1 && runtime.warmSurfaceShowCalls == 0 &&
                runtime.showOverlayCalls == 1 && capturePresentedCalls == 1 &&
                runtime.showOverlayModes.size() == 1 &&
                runtime.showOverlayModes.constLast() == ScreenshotOverlayShowMode::CapturedImage,
            "reveal must present the captured overlay once without an earlier surface warmup");
}

void displayChangesRefreshWithoutCancelingIdleOrActiveCapture() {
    ScreenshotCaptureState idleState;
    idleState.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession idleDisplays;
    ScreenshotGeometryMapper idleGeometry;
    ScreenshotInteractionState idleInteraction;
    ScreenshotSelectionModel idleSelection;
    ScreenshotIntelligentSelectionModel idleSmartSelection;
    CaptureRuntime idleRuntime;
    auto idleWorkflow = makeWorkflow(idleState, idleDisplays, idleGeometry, idleInteraction,
                                     idleSelection, idleSmartSelection, idleRuntime);

    idleWorkflow.handleDisplayConfigurationChanged();
    require(idleState.layoutDirty && idleRuntime.refreshLayoutCalls == 1 &&
                idleRuntime.cancelActiveCaptureCalls == 0,
            "idle display changes must schedule a refined refresh without cancellation or release");

    ScreenshotCaptureState activeState;
    activeState.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession activeDisplays;
    ScreenshotGeometryMapper activeGeometry;
    ScreenshotInteractionState activeInteraction;
    ScreenshotSelectionModel activeSelection;
    ScreenshotIntelligentSelectionModel activeSmartSelection;
    CaptureRuntime activeRuntime;
    auto activeWorkflow =
        makeWorkflow(activeState, activeDisplays, activeGeometry, activeInteraction,
                     activeSelection, activeSmartSelection, activeRuntime);
    activeWorkflow.startCapture();
    activeWorkflow.handleDisplayConfigurationChanged();
    require(activeState.captureInProgress && activeRuntime.refreshLayoutCalls == 0 &&
                activeRuntime.cancelActiveCaptureCalls == 0,
            "display changes during capture must leave capture running");

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    activeRuntime.eventSink->handleCaptureFinished(
        successfulResult(activeState.sessionId, snapshot));
    require(activeRuntime.refreshLayoutCalls == 1 && activeState.layoutDirty,
            "a display change during capture must refresh after capture completion");
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
            "enabled Smart selection must initially capture the deepest child element");
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
    require(selection.selectionTarget() == ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "capture without a supplied preference must retain the default child-element mode");

    selection.beginCaptureSession(false);
    require(!selection.smartSelectionEnabled() &&
                selection.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                !selection.toggleSelectionTarget() &&
                selection.applyCanvasHitPath({nestedElement, childElement, window}, bounds, 1.0) &&
                selection.index() == 2 && selection.currentSelection() == window,
            "disabled Smart selection must remain locked to window capture");

    require(selection.updateSmartSelectionEnabled(true) &&
                selection.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::WindowSubElement &&
                selection.index() == 0 && selection.currentSelection() == nestedElement &&
                selection.updateSmartSelectionEnabled(false) &&
                selection.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                selection.index() == 2 && selection.currentSelection() == window,
            "a live Smart selection setting change must immediately enforce its target policy");
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
    auto enabledWorkflow =
        makeWorkflow(enabledState, enabledDisplays, enabledGeometry, enabledInteraction,
                     enabledSelection, enabledIntelligentSelection, enabledRuntime, true);

    enabledWorkflow.startCapture();
    require(enabledIntelligentSelection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "an enabled capture session must begin in child-element mode");
    require(enabledIntelligentSelection.toggleSelectionTarget(),
            "enabled capture session must allow the target mode to switch");
    enabledWorkflow.startCapture();
    require(enabledIntelligentSelection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "capture without a persistence callback must retain the default child-element mode");

    ScreenshotCaptureState disabledState;
    disabledState.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession disabledDisplays;
    ScreenshotGeometryMapper disabledGeometry;
    ScreenshotInteractionState disabledInteraction;
    ScreenshotSelectionModel disabledSelection;
    ScreenshotIntelligentSelectionModel disabledIntelligentSelection;
    CaptureRuntime disabledRuntime;
    auto disabledWorkflow =
        makeWorkflow(disabledState, disabledDisplays, disabledGeometry, disabledInteraction,
                     disabledSelection, disabledIntelligentSelection, disabledRuntime, false);

    disabledWorkflow.startCapture();
    require(disabledIntelligentSelection.selectionTarget() ==
                    ScreenshotIntelligentSelectionTarget::Window &&
                !disabledIntelligentSelection.toggleSelectionTarget(),
            "a disabled capture session must stay in window mode");
}
} // namespace

void captureSnapshotsScreenColorSetting() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    bool enabled = false;
    bool captureCursor = false;
    ScreenshotCaptureWorkflowContext context{
        state, runtime, geometry, displays, interaction, selection, intelligentSelection, {}};
    context.restoreOriginalScreenColors = [&enabled]() { return enabled; };
    context.captureCursor = [&captureCursor]() { return captureCursor; };
    ScreenshotCaptureWorkflow workflow(context);
    workflow.startCapture();
    require(!runtime.lastCaptureRequest.restoreOriginalScreenColors &&
                !state.restoreOriginalScreenColors && !runtime.lastCaptureRequest.captureCursor &&
                !state.captureCursor,
            "capture must propagate disabled screenshot settings");
    enabled = true;
    captureCursor = true;
    require(!state.restoreOriginalScreenColors && !state.captureCursor,
            "active capture must retain its setting snapshot");
    workflow.startCapture();
    require(runtime.lastCaptureRequest.restoreOriginalScreenColors &&
                state.restoreOriginalScreenColors && runtime.lastCaptureRequest.captureCursor &&
                state.captureCursor,
            "the next capture must observe changed screenshot settings");
}

void toolbarVisibilityIsIndependentOfInputAndPreparation() {
    using Mode = ScreenshotCaptureWorkflow::StartMode;
    using Preparation = ScreenshotCaptureWorkflow::ToolbarPreparation;
    using Visibility = ScreenshotCaptureWorkflow::ToolbarVisibility;
    for (const auto mode : {Mode::Normal, Mode::ExternalDrag}) {
        for (const auto preparation : {Preparation::Prewarm, Preparation::OnDemand}) {
            ScreenshotCaptureState state;
            ScreenshotDisplaySession displays;
            ScreenshotGeometryMapper geometry;
            ScreenshotInteractionState interaction;
            ScreenshotSelectionModel selection;
            ScreenshotIntelligentSelectionModel intelligent;
            CaptureRuntime runtime;
            auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                         intelligent, runtime);
            for (const auto visibility : {Visibility::Suppressed, Visibility::ShowAfterSelection,
                                          Visibility::Suppressed, Visibility::ShowAfterSelection}) {
                workflow.startCapture(mode, preparation, visibility);
                CapturedDisplayModel snapshot;
                snapshot.stableId = QStringLiteral("primary");
                snapshot.physicalRect = QRect(0, 0, 64, 48);
                snapshot.logicalRect = snapshot.physicalRect;
                snapshot.image = QImage(64, 48, QImage::Format_RGBA8888);
                snapshot.image.fill(Qt::blue);
                runtime.eventSink->handleCaptureFinished(
                    successfulResult(state.sessionId, snapshot));
                selection.setSelectionStartEnd({10, 10}, {30, 20});
                interaction.finishDrag();
                interaction.confirmSelection();
                state.sessionState = ScreenshotSessionState::Editing;
                require(workflow.suppressCaptureToolbar() == (visibility == Visibility::Suppressed),
                        "confirmed selection must use this request's toolbar visibility regardless "
                        "of input mode, preparation strategy, or previous capture");
                workflow.cancelCapture();
            }
        }
    }
}

void normalCaptureRestoresToolbarAfterSuppressedCapture() {
    using Mode = ScreenshotCaptureWorkflow::StartMode;
    for (const auto mode : {Mode::ExternalDrag, Mode::Normal}) {
        for (const bool exportCleanup : {false, true}) {
            ScreenshotCaptureState state;
            ScreenshotDisplaySession displays;
            ScreenshotGeometryMapper geometry;
            ScreenshotInteractionState interaction;
            ScreenshotSelectionModel selection;
            ScreenshotIntelligentSelectionModel intelligent;
            CaptureRuntime runtime;
            auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                         intelligent, runtime);
            workflow.startCapture(mode, ScreenshotCaptureWorkflow::ToolbarPreparation::OnDemand,
                                  ScreenshotCaptureWorkflow::ToolbarVisibility::Suppressed);
            require(runtime.prewarmToolbarSurfaceCalls == 0,
                    "on-demand captures must not prepare an unused editing toolbar");
            require(workflow.suppressCaptureToolbar(),
                    "explicit suppression must hide the toolbar for either input mode");
            if (exportCleanup) {
                workflow.cancelCaptureForExport();
            } else {
                workflow.cancelCapture();
            }
            // History editing starts the workflow directly with its default mode.
            workflow.startCapture();
            require(!workflow.suppressCaptureToolbar(),
                    "a history edit must restore the toolbar after any suppressed capture");
            require(runtime.prewarmToolbarSurfaceCalls == 1,
                    "normal capture must restore toolbar prewarming after on-demand capture");
        }
    }
}

void noToolbarCaptureWaitsForUserSelection() {
    for (const bool preparedDisplay : {false, true}) {
        for (const bool smartSelectionEnabled : {false, true}) {
            ScreenshotCaptureState state;
            ScreenshotDisplaySession displays;
            ScreenshotGeometryMapper geometry;
            ScreenshotInteractionState interaction;
            ScreenshotSelectionModel selection;
            ScreenshotIntelligentSelectionModel intelligent;
            CaptureRuntime runtime;
            runtime.seedActiveDisplayOnPrepare = preparedDisplay;
            auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                         intelligent, runtime, smartSelectionEnabled);
            workflow.startCapture(ScreenshotCaptureWorkflow::StartMode::Normal,
                                  ScreenshotCaptureWorkflow::ToolbarPreparation::OnDemand,
                                  ScreenshotCaptureWorkflow::ToolbarVisibility::Suppressed);

            CapturedDisplayModel snapshot;
            snapshot.stableId = QStringLiteral("primary");
            snapshot.physicalRect = QRect(0, 0, 64, 48);
            snapshot.logicalRect = snapshot.physicalRect;
            snapshot.image = QImage(64, 48, QImage::Format_RGBA8888);
            snapshot.image.fill(Qt::blue);
            runtime.eventSink->handleCaptureFinished(successfulResult(state.sessionId, snapshot));

            require(runtime.capturedImageShowCalls == 1 && !interaction.inactive(),
                    "no-toolbar hotkey capture must present the selection overlay");
            require(!interaction.dragging() && !selection.hasPixelSelection(),
                    "no-toolbar hotkey capture must wait for a press before starting a drag");
            require(intelligent.smartSelectionEnabled() == smartSelectionEnabled,
                    "hiding the toolbar must preserve the configured smart-selection behavior");
            require(runtime.startWorkflowRefreshCalls > 0,
                    "no-toolbar hotkey capture must initialize cursor selection normally");
            require(runtime.prewarmToolbarSurfaceCalls == 0,
                    "on-demand capture must not prepare the toolbar during presentation");
        }
    }
}

void externalDragBypassesSelectorAndPreparesBeforeReveal() {
    for (const bool cancelBeforeReveal : {false, true}) {
        ScreenshotCaptureState state;
        state.sessionState = ScreenshotSessionState::IdlePrepared;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligent;
        CaptureRuntime runtime;
        runtime.seedActiveDisplayOnPrepare = true;
        runtime.acceptSelectorHitTest = true;
        ScreenshotCaptureWorkflowContext context{state,       runtime,   geometry,    displays,
                                                 interaction, selection, intelligent, {}};
        ScreenshotCaptureWorkflow* active = nullptr;
        int prepared = 0;
        int presented = 0;
        context.presentation.beforeCapturePresented = [&]() {
            require(runtime.showOverlayCalls == 0 && !geometry.isEmpty(),
                    "external selection must prepare against final geometry before reveal");
            ++prepared;
            selection.setSelectionStartEnd({10, 10}, {30, 20});
            if (cancelBeforeReveal)
                active->cancelCapture();
        };
        context.presentation.capturePresented = [&]() {
            ++presented;
            require(prepared == 1 && selection.pixelSelection() == QRect(10, 10, 20, 10),
                    "first presentation must retain the supplied drag rectangle");
        };
        ScreenshotCaptureWorkflow workflow(context);
        active = &workflow;
        workflow.startCapture(ScreenshotCaptureWorkflow::StartMode::ExternalDrag);
        require(runtime.prewarmToolbarSurfaceCalls == 1,
                "external drags entering editing must retain toolbar prewarming");
        require(runtime.startWorkflowRefreshCalls == 0 && interaction.manualSelecting(),
                "global mouse capture must not initialize or wait on smart selection");
        CapturedDisplayModel snapshot;
        snapshot.stableId = QStringLiteral("primary");
        snapshot.name = QStringLiteral("Primary");
        snapshot.physicalRect = QRect(0, 0, 64, 48);
        snapshot.logicalRect = snapshot.physicalRect;
        snapshot.image = QImage(64, 48, QImage::Format_RGBA8888);
        snapshot.image.fill(Qt::blue);
        const auto result = successfulResult(state.sessionId, snapshot);
        runtime.eventSink->handleCaptureFinished(result);
        runtime.eventSink->handleCaptureFinished(result);
        require(prepared == 1 && presented == (cancelBeforeReveal ? 0 : 1) &&
                    runtime.capturedImageShowCalls == (cancelBeforeReveal ? 0 : 1),
                "external capture must present once, or remain hidden after cancellation");
        require(runtime.startWorkflowRefreshCalls == 0,
                "external capture presentation must not launch a late selector refresh");
        if (!cancelBeforeReveal) {
            interaction.finishDrag();
            interaction.confirmSelection();
            state.sessionState = ScreenshotSessionState::Editing;
            require(!workflow.suppressCaptureToolbar(),
                    "external drags entering editing must allow the screenshot toolbar");
            const quint64 completedSession = state.sessionId;
            workflow.handleDisplayConfigurationChanged();
            require(
                state.sessionId == completedSession && !interaction.inactive(),
                "completed global selections must retain normal editing display-change behavior");
        }
    }
}

void externalDragDisplayChangesInvalidatePendingCapture() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::IdlePrepared;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    CaptureRuntime runtime;
    int terminated = 0;
    ScreenshotCaptureWorkflowContext context{state,       runtime,   geometry,    displays,
                                             interaction, selection, intelligent, {}};
    context.captureTerminated = [&]() { ++terminated; };
    ScreenshotCaptureWorkflow workflow(context);
    workflow.startCapture(ScreenshotCaptureWorkflow::StartMode::ExternalDrag);
    const quint64 pending = state.sessionId;
    workflow.handleDisplayConfigurationChanged();
    require(terminated == 1 && state.sessionId != pending && interaction.inactive(),
            "display changes must cancel a global drag before its geometry becomes stale");
    CapturedDisplayModel snapshot;
    snapshot.physicalRect = QRect(0, 0, 100, 100);
    snapshot.image = QImage(100, 100, QImage::Format_RGBA8888);
    snapshot.image.fill(Qt::red);
    runtime.eventSink->handleCaptureFinished(successfulResult(pending, snapshot));
    require(runtime.capturedImageShowCalls == 0 && interaction.inactive(),
            "cancelled global capture results must never reopen the overlay");
}

void globalDragCoordinatesStayPhysicalAcrossDifferentDisplayScales() {
    ScreenshotDisplaySession displays;
    CapturedDisplayModel left;
    left.active = true;
    left.physicalRect = QRect(-1920, -200, 1920, 1080);
    left.canvasRect = QRect(0, 0, 1920, 1080);
    left.logicalRect = QRect(-1536, -160, 1536, 864);
    CapturedDisplayModel right;
    right.active = true;
    right.physicalRect = QRect(0, 0, 2560, 1440);
    right.canvasRect = QRect(1920, 200, 2560, 1440);
    right.logicalRect = QRect(0, 0, 1707, 960);
    displays.appendDisplay(left);
    displays.appendDisplay(right);
    ScreenshotGeometryMapper geometry;
    const QPointF start = geometry.canvasPositionForPhysicalPoint(displays, {-100, -100});
    const QPointF end = geometry.canvasPositionForPhysicalPoint(displays, {1500, 900});
    require(start == QPointF(1820, 100) && end == QPointF(3420, 1100) &&
                QRectF(start, end).size() == QSizeF(1600, 1000),
            "mixed-DPI drags must retain physical pixel dimensions and negative monitor origins");
}

int main() {
    toolbarVisibilityIsIndependentOfInputAndPreparation();
    noToolbarCaptureWaitsForUserSelection();
    normalCaptureRestoresToolbarAfterSuppressedCapture();
    externalDragDisplayChangesInvalidatePendingCapture();
    globalDragCoordinatesStayPhysicalAcrossDifferentDisplayScales();
    externalDragBypassesSelectorAndPreparesBeforeReveal();
    captureSnapshotsScreenColorSetting();
    captureRestoresSelectionEffectsAfterReset();
    idlePrewarmDoesNotInitializeSelector();
    endingScreenshotReprewarmsOverlaySurfaces();
    cancelConcealsOverlayBeforeClearingVisibleFrame();
    exportCancellationDefersExpensiveCleanup();
    captureOverlapsSelectorInitialization();
    synchronousCaptureFailureDoesNotRestartSelectorRefresh();
    restartingCaptureReleasesPreviousSelectorCache();
    capturePresentedRunsAfterCapturedOverlayIsShown();
    capturedOverlayWaitsForImageAndSelectionInEitherOrder();
    overlayCapturePrewarmsToolbarSurfaceAfterCaptureDispatch();
    overlayCaptureDoesNotWarmNativeSurfaceAfterCaptureDispatch();
    displayChangesRefreshWithoutCancelingIdleOrActiveCapture();
    capturedImagePlacementFollowsNormalizedCanvasGeometry();
    intelligentSelectionTargetsPreserveElementPathBehavior();
    captureSessionsApplyTheCurrentSmartSelectionSetting();
    return 0;
}
