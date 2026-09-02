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
        return true;
    }
    [[nodiscard]] bool hasCaptureWorker() const override {
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
        return false;
    }

    void prewarmDisplayPool(ScreenshotDisplaySession&, int) override {
        ++prewarmDisplayPoolCalls;
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
    [[nodiscard]] bool preparePreCaptureOverlayWindows(ScreenshotDisplaySession&) override {
        ++preparePreCaptureOverlayCalls;
        return true;
    }
    void showOverlayWindows(const ScreenshotDisplaySession&, ScreenshotOverlayShowMode) override {
        ++showOverlayCalls;
    }
    void hideOverlayWindowsImmediately(const ScreenshotDisplaySession&) override {
        ++hideOverlayImmediatelyCalls;
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
    int captureAllAsyncCalls = 0;
    int cancelActiveCaptureCalls = 0;
    int refreshLayoutCalls = 0;
    quint64 lastRefreshRequestId = 0;
    int startWorkflowRefreshCalls = 0;
    int releaseSelectorCacheCalls = 0;
    mutable int clearOverlayCanvasCalls = 0;
    int clearDisplayCalls = 0;
    int showOverlayCalls = 0;
    int applyDisplayModelsCalls = 0;
    int preparePreCaptureOverlayCalls = 0;
    int hideOverlayCalls = 0;
    int hideOverlayImmediatelyCalls = 0;
    int resetForNewCaptureCalls = 0;
    int clearDocumentCalls = 0;
    int prewarmDisplayPoolCalls = 0;
    bool selectorIsReady = false;
    bool selectorRefreshActive = false;
    bool captureWasQueuedBeforeSelectorRefresh = false;
    bool failCaptureSynchronously = false;
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
    require(runtime.prepareAsyncCalls == 1 && runtime.prewarmDisplayPoolCalls == 1,
            "idle kernel preparation must be idempotent once resources are prepared");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared,
            "idle prewarm must leave the workflow prepared");
    require(runtime.startWorkflowRefreshCalls == 0 && !runtime.selectorRefreshActive,
            "idle prewarm must not initialize the selector cache");

    workflow.startCapture();
    workflow.prewarmResources();
    require(runtime.startWorkflowRefreshCalls == 1 && runtime.selectorRefreshActive,
            "capture start must initialize the selector cache");
}

void endingScreenshotSkipsCaptureReleaseAndPrewarm() {
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
    require(runtime.prepareAsyncCalls == 0 && runtime.prewarmDisplayPoolCalls == 0,
            "ending a screenshot must not prepare or prewarm capture resources");
    require(captureTerminatedCalls == 1,
            "canceling a capture must stop active capture-scoped features before cleanup");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared,
            "canceling a capture must return the workflow to its prepared idle state");
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
    require(state.sessionState == ScreenshotSessionState::IdlePrepared &&
                !state.captureInProgress,
            "export cancellation must leave the workflow ready for presentation");

    workflow.completeDeferredExportCleanup();
    require(runtime.resetForNewCaptureCalls == 1,
            "deferred export cleanup must perform the capture reset later");
    workflow.completeDeferredExportCleanup();
    require(runtime.resetForNewCaptureCalls == 1,
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
                    runtime.captureWasQueuedBeforeSelectorRefresh &&
                    runtime.selectorRefreshActive,
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

    require(!state.captureInProgress &&
                state.sessionState == ScreenshotSessionState::IdlePrepared,
            "synchronous capture failure must return the workflow to idle");
    require(runtime.startWorkflowRefreshCalls == 0 && !runtime.selectorRefreshActive,
            "synchronous capture failure must not restart selector refresh after cleanup");
    require(runtime.prepareAsyncCalls == 0 && runtime.prewarmDisplayPoolCalls == 0,
            "synchronous failure cleanup must not release or prewarm capture resources");
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

    require(runtime.showOverlayCalls == 1 && capturePresentedCalls == 1 &&
                showCallsObservedByCallback == 1,
            "capture-presented callback must run once after the captured overlay is shown");
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
    auto activeWorkflow = makeWorkflow(activeState, activeDisplays, activeGeometry, activeInteraction,
                                       activeSelection, activeSmartSelection, activeRuntime);
    activeWorkflow.startCapture(ScreenshotCapturePresentationMode::Silent);
    activeWorkflow.handleDisplayConfigurationChanged();
    require(activeState.captureInProgress && activeRuntime.refreshLayoutCalls == 0 &&
                activeRuntime.cancelActiveCaptureCalls == 0,
            "display changes during capture must leave capture running");

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    activeRuntime.eventSink->handleCaptureFinished(successfulResult(activeState.sessionId, snapshot));
    require(activeRuntime.refreshLayoutCalls == 1 && activeState.layoutDirty,
            "a display change during capture must refresh after capture completion");
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

    require(!state.captureInProgress &&
                state.sessionState == ScreenshotSessionState::IdlePrepared &&
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
    require(selection.selectionTarget() ==
                ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "a new screenshot must discard the preceding screenshot's Tab target mode");

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
    endingScreenshotSkipsCaptureReleaseAndPrewarm();
    exportCancellationDefersExpensiveCleanup();
    captureOverlapsSelectorInitialization();
    synchronousCaptureFailureDoesNotRestartSelectorRefresh();
    restartingCaptureReleasesPreviousSelectorCache();
    capturePresentedRunsAfterCapturedOverlayIsShown();
    silentCaptureNeverPreparesOrShowsOverlays();
    displayChangesRefreshWithoutCancelingIdleOrActiveCapture();
    focusedWindowCaptureUsesOneCompoundWorkerTransaction();
    focusedWindowCaptureIsAllOrNothing();
    capturedImagePlacementFollowsNormalizedCanvasGeometry();
    intelligentSelectionTargetsPreserveElementPathBehavior();
    captureSessionsApplyTheCurrentSmartSelectionSetting();
    return 0;
}
