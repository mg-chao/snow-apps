#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h"
#include "snow_shot/presentation/screenshotcaptureworkflow.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectorworkflow.h"
#include "snow_shot/presentation/screenshottoolbarpresentationstatefactory.h"

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
        operations.push_back(QStringLiteral("capture-dispatched"));
        lastCaptureRequest = request;
        captureWasQueuedBeforeSelectorRefresh = !selectorRefreshActive;
        if (!failCaptureSynchronously && seedActiveDisplayOnPrepare &&
            request.purpose == ScreenshotCapturePurpose::Initial) {
            deliverLayout();
        }
        if (failCaptureSynchronously && eventSink != nullptr) {
            ScreenshotCaptureResult result;
            result.requestId = request.requestId;
            result.purpose = request.purpose;
            result.errorMessage = QStringLiteral("Synchronous capture setup failure");
            eventSink->handleCaptureFinished(result);
        }
    }
    void deliverLayout() {
        ScreenshotCaptureLayout layout;
        layout.requestId = lastCaptureRequest.requestId;
        layout.generation = layout.requestId;
        preparedDisplays->forEachActiveDisplay(
            [&](qsizetype, const CapturedDisplayModel& d) { layout.displays.push_back(d); });
        eventSink->handleLayoutReady(layout);
    }
    void deliverResult(const ScreenshotCaptureResult& result) {
        if (result.purpose == ScreenshotCapturePurpose::Initial && result.succeeded &&
            preparedDisplays && preparedDisplays->startup &&
            preparedDisplays->startup->phase == ScreenshotStartupContext::Phase::Preparing &&
            !preparedDisplays->startup->displays &&
            result.requestId == lastCaptureRequest.requestId)
            deliverLayout();
        eventSink->handleCaptureFinished(result);
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
    [[nodiscard]] bool updateSelectorSelectionAt(const QPoint& point) override {
        queriedPoint = point;
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
        operations.push_back(QStringLiteral("prepare-overlays"));
        preparedDisplays = &displaySession;
        displaySession.clear();
        {
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
        operations.push_back(QStringLiteral("show-overlays"));
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
    void releaseSelectionPreviewCache() override {
        ++releaseSelectionPreviewCacheCalls;
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
    void createColorPicker(const QPoint&) override {
        ++createColorPickerCalls;
        colorPickerAlive = true;
        operations.push_back(QStringLiteral("create-picker"));
    }
    void prepareColorPickerSurface(const ScreenshotDisplaySession&) override {
        ++prepareColorPickerCalls;
        operations.push_back(QStringLiteral("prepare-picker"));
    }
    void releaseColorPicker() override {
        ++releaseColorPickerCalls;
        colorPickerAlive = false;
        operations.push_back(QStringLiteral("release-picker"));
    }

    int createColorPickerCalls = 0;
    int prepareColorPickerCalls = 0;
    int releaseColorPickerCalls = 0;
    bool colorPickerAlive = false;

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
    int releaseSelectionPreviewCacheCalls = 0;
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
    bool seedActiveDisplayOnPrepare = true;
    ScreenshotDisplaySession* preparedDisplays = nullptr;
    QPoint queriedPoint;
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

ScreenshotCaptureResult successfulRecaptureResult(quint64 requestId,
                                                  const CapturedDisplayModel& snapshot) {
    ScreenshotCaptureResult result = successfulResult(requestId, snapshot);
    result.purpose = ScreenshotCapturePurpose::Recapture;
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

void confirmedSelectionPreservesRegionTypeInToolbarPresentation() {
    for (auto type : {ScreenshotRegionType::Rectangle, ScreenshotRegionType::Polyline,
                      ScreenshotRegionType::Curve, ScreenshotRegionType::Freehand}) {
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        interaction.beginCapture();
        selection.setRegionType(type);
        selection.setSelectionRect(QRectF(10, 20, 100, 80));
        interaction.confirmSelection();
        const auto state = makeScreenshotToolbarPresentationState(interaction, selection);
        require(state.regionType == type,
                "confirmation passes the active area tool to the toolbar, independent of geometry");
    }
}

void captureRestoresSelectionPreferencesAfterReset() {
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
        bool aspectRatioLocked = true;
        auto regionType = ScreenshotRegionType::Polyline;
        context.restoreSelectionPreferences = [&]() {
            selection.setRegionType(regionType);
            static_cast<void>(selection.setCornerRadius(radius));
            static_cast<void>(selection.setShadowWidth(shadowWidth));
            static_cast<void>(selection.setAspectRatioLockEnabled(aspectRatioLocked, 5.0));
        };
        ScreenshotCaptureWorkflow workflow(std::move(context));
        if (prewarm) {
            workflow.prewarmResources();
        }
        workflow.startCapture();
        require(selection.regionType() == regionType,
                "cold and prewarmed captures restore the saved region type");
        require(selection.cornerRadius() == 24 && selection.shadowWidth() == 12,
                "cold and prewarmed captures must restore effects after resetting the model");
        require(!selection.hasPixelSelection() && selection.aspectRatioLocked(),
                "capture startup must restore the lock preference without restoring geometry");
        workflow.cancelCapture();
        radius = 32;
        shadowWidth = 16;
        regionType = ScreenshotRegionType::Curve;
        workflow.startCapture();
        require(selection.regionType() == regionType,
                "captures after cancellation reload the latest region type");
        require(selection.cornerRadius() == 32 && selection.shadowWidth() == 16,
                "captures after cancellation must reload the latest saved effects");
        radius = 0;
        shadowWidth = 0;
        aspectRatioLocked = false;
        regionType = ScreenshotRegionType::Freehand;
        workflow.startCapture();
        require(selection.regionType() == regionType,
                "restarting an active capture reloads the latest region type");
        require(selection.cornerRadius() == 0 && selection.shadowWidth() == 0 &&
                    !selection.aspectRatioLocked(),
                "restarting an active capture must restore disabled selection preferences");
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
    require(runtime.createColorPickerCalls == 0 && runtime.prepareColorPickerCalls == 0,
            "idle prewarm must not create or prepare the color picker");
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
    require(runtime.releaseSelectionPreviewCacheCalls == 1,
            "canceling a capture must release its shadow and checkerboard preview cache");
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
    require(runtime.releaseColorPickerCalls == 1 && !runtime.colorPickerAlive,
            "export must release the picker before deferred cleanup");
    require(runtime.cancelActiveCaptureCalls == 1 && captureTerminatedCalls == 1,
            "export cancellation must stop the active capture before presenting the pin");
    require(runtime.hideOverlayImmediatelyCalls == 1 && runtime.hideOverlayCalls == 0,
            "export cancellation must use the immediate overlay hide path");
    require(runtime.resetForNewCaptureCalls == 0,
            "export cancellation must defer the expensive capture reset");
    require(runtime.prewarmDisplayPoolCalls == 0,
            "export cancellation must defer overlay surface prewarming with cleanup");
    require(runtime.releaseSelectionPreviewCacheCalls == 0,
            "export cancellation must preserve preview assets until deferred cleanup");
    require(state.sessionState == ScreenshotSessionState::IdlePrepared && !state.captureInProgress,
            "export cancellation must leave the workflow ready for presentation");

    workflow.completeDeferredExportCleanup();
    require(runtime.resetForNewCaptureCalls == 1,
            "deferred export cleanup must perform the capture reset later");
    require(runtime.prewarmDisplayPoolCalls == 1 && runtime.prewarmDisplayPoolSawRuntimeReset,
            "deferred export cleanup must re-prewarm overlay surfaces after runtime cleanup");
    require(runtime.releaseSelectionPreviewCacheCalls == 1,
            "deferred export cleanup must release shadow and checkerboard preview assets");
    workflow.completeDeferredExportCleanup();
    require(runtime.resetForNewCaptureCalls == 1 && runtime.prewarmDisplayPoolCalls == 1,
            "deferred export cleanup must be idempotent");
    require(runtime.releaseSelectionPreviewCacheCalls == 1,
            "deferred export cleanup must release preview assets only once");
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

    require(runtime.createColorPickerCalls == 1 && runtime.releaseColorPickerCalls == 1 &&
                runtime.prepareColorPickerCalls == 0 && !runtime.colorPickerAlive,
            "synchronous failure must destroy the picker and skip subsequent preparation");
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
    require(runtime.releaseSelectionPreviewCacheCalls == 1,
            "starting a new capture must release the previous preview assets");
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
    runtime.deliverResult(result);
    runtime.deliverResult(result);

    require(runtime.prepareColorPickerCalls == 1 &&
                runtime.operations.lastIndexOf(QStringLiteral("prepare-picker")) <
                    runtime.operations.indexOf(QStringLiteral("show-overlays")),
            "displays supplied by the capture result must prepare the picker before presentation");
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
            runtime.deliverResult(result);
        }
        require(runtime.showOverlayCalls == 0,
                "neither the image nor the selection alone may show or warm the overlay");
        workflow.handleInitialSmartSelectionResolved(state.sessionId + 1);
        require(runtime.showOverlayCalls == 0,
                "a different session's selection must not release the reveal gate");

        if (selectionFirst) {
            runtime.deliverResult(result);
        } else {
            workflow.handleInitialSmartSelectionResolved(state.sessionId);
        }
        require(runtime.showOverlayCalls == 1 && runtime.capturedImageShowCalls == 1 &&
                    runtime.applyDisplayModelsCalls == 1,
                "image and selection readiness must reveal the prepared overlay once");
        workflow.handleInitialSmartSelectionResolved(state.sessionId);
        runtime.deliverResult(result);
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
    runtime.deliverResult(successfulResult(state.sessionId, snapshot));

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
    require(!activeState.captureInProgress && activeRuntime.refreshLayoutCalls == 1 &&
                activeRuntime.cancelActiveCaptureCalls == 1,
            "display changes during startup must cancel capture and refresh layout");

    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.name = QStringLiteral("Primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGBA8888);
    activeRuntime.deliverResult(successfulResult(activeState.sessionId, snapshot));
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

void phasedWorkflowOnlySignalsInitialReadinessOnce() {
    class Service final : public ScreenshotSelectorServicePort {
      public:
        bool ready() const override {
            return true;
        }
        bool refreshInFlight() const override {
            return false;
        }
        bool startRefresh(const QVector<std::uintptr_t>&) override {
            return true;
        }
        bool requestHitTest(const QPoint&, ScreenshotSelectorHitTestMode) override {
            return true;
        }
    } service;
    class Exclusions final : public ScreenshotOverlayExclusionPort {
      public:
        QVector<std::uintptr_t> excludedHwnds(const ScreenshotDisplaySession&) const override {
            return {};
        }
    } exclusions;
    ScreenshotCaptureState state;
    state.sessionId = 10;
    ScreenshotDisplaySession displays;
    CapturedDisplayModel display;
    display.stableId = QStringLiteral("display:1");
    display.physicalRect = QRect(0, 0, 200, 200);
    display.capturedLogicalRect = QRect(0, 0, 100, 100);
    display.nativeDisplayId = 1;
    display.backingScale = 2;
    display.canvasUsesPoints = true;
    display.active = true;
    displays.appendDisplay(display);
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    ScreenshotInteractionState interaction;
    interaction.beginCapture();
    interaction.enterOverlayVisible(true);
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    intelligent.beginCaptureSession(true);
    int readyCount = 0, updates = 0;
    ScreenshotSelectorPresentationCallbacks callbacks;
    callbacks.updateOverlayState = [&]() { ++updates; };
    callbacks.smartSelectionResultReady = [&](quint64 session) {
        require(session == state.sessionId, "wrong readiness session");
        ++readyCount;
    };
    ScreenshotSelectorWorkflow workflow({state, service, exclusions, displays, geometry,
                                         interaction, selection, intelligent, callbacks});
    const QRectF bounds(0, 0, 100, 100), parent(0, 0, 80, 80), child(0, 0, 40, 40),
        leaf(0, 0, 20, 20);
    const auto physical = [](const QRectF& rect) {
        return QRectF(rect.topLeft() * 2, rect.size() * 2);
    };
    selection.setRegionType(ScreenshotRegionType::Curve);
    workflow.handleInitialResult(true, {physical(bounds)}, 1);
    workflow.handleRefreshFinished(true);
    workflow.handleRefinement({physical(bounds)}, 1, true);
    require(!workflow.requestHitTest(QPoint(10, 10), 1) && selection.pixelSelection().isEmpty() &&
                readyCount == 0 && updates == 0,
            "custom mode suppresses requests and stale selector replies");
    selection.setRegionType(ScreenshotRegionType::Rectangle);
    workflow.handleInitialResult(true, {physical(bounds)}, 1);
    require(intelligent.currentSelection() == bounds,
            "permission fallback must apply the window on the queried display");
    // The first result arrives while the image is still being acquired and the
    // pointer remains stationary. Image arrival must preserve that selection.
    display.image = QImage(200, 200, QImage::Format_RGBA8888);
    ScreenshotCaptureDisplayModelReconciler::applySnapshots(displays, {display});
    geometry.rebuild(displays);
    require(selection.normalizedSelection() == bounds,
            "image arrival must preserve the selection resolved on the prepared Retina canvas");
    workflow.handleInitialResult(true, {physical(parent), physical(bounds)}, 1);
    workflow.handleRefinement({physical(child), physical(parent), physical(bounds)}, 1);
    require(readyCount == 1 && updates == 3 && intelligent.currentSelection() == child,
            "refinement must update presentation without repeating initial readiness");
    intelligent.beginPress(QPointF(5, 5), child);
    workflow.handleRefinement(
        {physical(leaf), physical(child), physical(parent), physical(bounds)});
    workflow.handleRefinement({physical(bounds)}, 1, true);
    require(updates == 3 && intelligent.takePressSelection() == child,
            "press must suppress late refinement");
    interaction.confirmSelection();
    workflow.handleRefinement(
        {physical(leaf), physical(child), physical(parent), physical(bounds)});
    require(updates == 3, "editing must suppress late refinement");
    ++state.sessionId;
    interaction.returnToSelectionMode(true);
    workflow.handleInitialResult(true, {physical(parent), physical(bounds)});
    require(readyCount == 2, "a new capture must notify readiness again");
    workflow.handleRefinement({physical(bounds)}, 1, true);
    require(intelligent.currentSelection() == bounds && readyCount == 2,
            "permission revocation during refinement must replace children with the window");
    ++state.sessionId;
    interaction.returnToSelectionMode(true);
    workflow.handleRefreshFinished(false);
    require(interaction.manualSelecting() && readyCount == 3,
            "failed selector refresh must switch to manual selection and unblock initial reveal");
    workflow.handleRefreshFinished(false);
    require(readyCount == 3, "failed refresh must resolve initial readiness only once");
}

void phasedSelectionPreservesUserIntent() {
    ScreenshotIntelligentSelectionModel model;
    const QRectF bounds(0, 0, 100, 100), parent(0, 0, 80, 80), child(0, 0, 40, 40),
        leaf(0, 0, 20, 20);
    model.beginCaptureSession(true);
    require(model.applyCanvasHitPath({parent, bounds}, bounds, 1), "initial path failed");
    require(model.applyCanvasRefinementPath({child, parent, bounds}, bounds, 1) &&
                model.currentSelection() == child,
            "stationary refinement must automatically deepen the marquee");
    require(!model.applyCanvasRefinementPath({child, parent, bounds}, bounds, 1),
            "duplicate refinement must not repaint");
    require(
        !model.applyCanvasRefinementPath({leaf, child, QRectF(0, 0, 90, 90), bounds}, bounds, 1),
        "a different ancestry must not replace the displayed path");
    require(model.selectIndex(1), "ancestor selection failed");
    require(model.applyCanvasRefinementPath({leaf, child, parent, bounds}, bounds, 1) &&
                model.currentSelection() == parent && model.index() == 2,
            "refinement must preserve an explicitly chosen rectangle by geometry");
    model.beginPress(QPointF(5, 5), parent);
    require(!model.applyCanvasRefinementPath({QRectF(0, 0, 10, 10), leaf, child, parent, bounds},
                                             bounds, 1) &&
                model.takePressSelection() == parent,
            "press must freeze refinement and confirmation geometry");
    model.resetTargetPreference();
    require(model.applyCanvasRefinementPath({QRectF(0, 0, 10, 10), leaf, child, parent, bounds},
                                            bounds, 1) &&
                model.index() == 0,
            "a target change must restore automatic selection");
    require(
        !model.applyCanvasRefinementPath(
            {QRectF(0, 0, 0.5, 0.5), QRectF(0, 0, 10, 10), leaf, child, parent, bounds}, bounds, 1),
        "minimum-size filtering must happen before extension comparison");
}

void intelligentSelectionTargetsPreserveElementPathBehavior() {
#ifdef Q_OS_MACOS
    {
        ScreenshotIntelligentSelectionModel model;
        const QRectF window(0, 0, 100, 100), pane(0, 0, 80, 80), text(10, 10, 20, 20);
        model.beginCaptureSession(true);
        require(model.applyCanvasHitPath({text, window}, window, 1), "initial AX path failed");
        require(model.selectIndex(1), "explicit AX window selection failed");
        require(model.applyCanvasRefinementPath({text, pane, window}, window, 1) &&
                    model.currentSelection() == window,
                "new AX containers must preserve an explicitly chosen enclosing frame");
    }
#endif
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

void initialCaptureSnapshotsScreenshotSettings() {
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
            "normal capture must honor the enabled cursor setting with smart selection");
    workflow.startCapture(ScreenshotCaptureWorkflow::StartMode::ExternalDrag);
    require(runtime.lastCaptureRequest.restoreOriginalScreenColors &&
                runtime.lastCaptureRequest.captureCursor && state.captureCursor,
            "external region drags must also observe the cursor setting");
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
                runtime.deliverResult(successfulResult(state.sessionId, snapshot));
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
            runtime.deliverResult(successfulResult(state.sessionId, snapshot));

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
            require(prepared == 1 && selection.pixelSelection() == QRect(10, 10, 21, 11),
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
        runtime.deliverResult(result);
        runtime.deliverResult(result);
        const bool framePacedReveal =
            !cancelBeforeReveal && runtime.showOverlayModes.size() == 1 &&
            runtime.showOverlayModes.first() == ScreenshotOverlayShowMode::CapturedImageFramePaced;
        require(prepared == 1 && presented == (cancelBeforeReveal ? 0 : 1) &&
                    runtime.showOverlayCalls == (cancelBeforeReveal ? 0 : 1) &&
                    (cancelBeforeReveal || framePacedReveal),
                "external capture must present once frame-paced, or stay hidden after cancel");
        require(runtime.capturedImageShowCalls == 0,
                "frame-paced external reveals must not take the synchronous reveal path");
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
    runtime.deliverResult(successfulResult(pending, snapshot));
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

void recapturePreservesEditingStateAndRollsBackFailures() {
    ScreenshotCaptureState state;
    state.sessionState = ScreenshotSessionState::Editing;
    ScreenshotDisplaySession displays;
    CapturedDisplayModel original;
    original.stableId = QStringLiteral("primary");
    original.name = QStringLiteral("Primary");
    original.physicalRect = QRect(0, 0, 64, 48);
    original.logicalRect = original.physicalRect;
    original.image = QImage(64, 48, QImage::Format_RGBA8888);
    original.image.fill(Qt::red);
    original.active = true;
    displays.appendDisplay(original);
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    ScreenshotInteractionState interaction;
    interaction.setMoveTool(true, false);
    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd({10, 8}, {30, 20});
    ScreenshotIntelligentSelectionModel intelligent;
    CaptureRuntime runtime;
    bool captureCursor = true;
    int completions = 0;
    bool lastSucceeded = false;
    ScreenshotCaptureWorkflowContext context{state,       runtime,   geometry,    displays,
                                             interaction, selection, intelligent, {}};
    context.captureCursor = [&captureCursor]() { return captureCursor; };
    context.recaptureCompleted = [&](bool succeeded, const QString&) {
        ++completions;
        lastSucceeded = succeeded;
    };
    ScreenshotCaptureWorkflow workflow(context);

    QVector<std::uint32_t> excludedWindowIds{101, 202};
    require(workflow.startRecapture(excludedWindowIds) && workflow.recaptureInProgress() &&
                runtime.lastCaptureRequest.purpose == ScreenshotCapturePurpose::Recapture &&
                runtime.lastCaptureRequest.captureCursor &&
                runtime.lastCaptureRequest.refreshLayout,
            "recapture must dispatch a distinct request with the current cursor setting");
    excludedWindowIds.clear();
    require(runtime.lastCaptureRequest.excludedWindowIds == QVector<std::uint32_t>{101, 202},
            "recapture must own both overlay and toolbar exclusions after dispatch");
    const QRect selectionBefore = selection.pixelSelection();
    const ScreenshotCaptureMode modeBefore = interaction.mode();
    CapturedDisplayModel replacement = original;
    replacement.image.fill(Qt::blue);
    runtime.deliverResult(
        successfulRecaptureResult(runtime.lastCaptureRequest.requestId, replacement));
    require(runtime.createColorPickerCalls == 0 && runtime.releaseColorPickerCalls == 0,
            "recapture must leave the existing session picker lifetime unchanged");
    require(completions == 1 && lastSucceeded && !workflow.recaptureInProgress() &&
                displays.displayAt(0).image.pixelColor(0, 0) == QColor(Qt::blue),
            "successful recapture must replace the desktop image exactly once");
    require(selection.pixelSelection() == selectionBefore && interaction.mode() == modeBefore &&
                interaction.moveToolActive() && runtime.clearDocumentCalls == 0 &&
                runtime.applyDisplayModelsCalls == 1,
            "successful recapture must preserve selection, interaction, and canvas state");

    captureCursor = false;
    require(workflow.startRecapture() && !runtime.lastCaptureRequest.captureCursor &&
                runtime.lastCaptureRequest.excludedWindowIds.isEmpty(),
            "each recapture must read the latest cursor setting");
    ScreenshotCaptureResult failed;
    failed.requestId = runtime.lastCaptureRequest.requestId;
    failed.purpose = ScreenshotCapturePurpose::Recapture;
    failed.errorMessage = QStringLiteral("capture failed");
    runtime.deliverResult(failed);
    require(completions == 2 && !lastSucceeded &&
                displays.displayAt(0).image.pixelColor(0, 0) == QColor(Qt::blue) &&
                state.sessionState == ScreenshotSessionState::Editing &&
                selection.pixelSelection() == selectionBefore &&
                runtime.applyDisplayModelsCalls == 1,
            "failed recapture must retain the previous image and complete editing state");

    require(workflow.startRecapture(), "recapture must recover after a failure");
    ScreenshotCaptureResult empty;
    empty.requestId = runtime.lastCaptureRequest.requestId;
    empty.purpose = ScreenshotCapturePurpose::Recapture;
    empty.succeeded = true;
    runtime.deliverResult(empty);
    require(completions == 3 && !lastSucceeded &&
                displays.displayAt(0).image.pixelColor(0, 0) == QColor(Qt::blue),
            "empty successful results must be rejected without clearing the old image");

    require(workflow.startRecapture(), "recapture must start before stale-result validation");
    const quint64 pendingRequestId = runtime.lastCaptureRequest.requestId;
    ScreenshotCaptureResult wrongPurpose = successfulRecaptureResult(pendingRequestId, replacement);
    wrongPurpose.purpose = ScreenshotCapturePurpose::Initial;
    runtime.deliverResult(wrongPurpose);
    require(workflow.recaptureInProgress() && completions == 3,
            "an initial-capture result must not complete a pending recapture");
    workflow.shutdownCaptureWorker();
    require(!workflow.recaptureInProgress() && completions == 4 && !lastSucceeded &&
                displays.displayAt(0).image.pixelColor(0, 0) == QColor(Qt::blue),
            "worker teardown must cancel recapture without changing the current image");
    runtime.deliverResult(successfulRecaptureResult(pendingRequestId, replacement));
    require(completions == 4 && displays.displayAt(0).image.pixelColor(0, 0) == QColor(Qt::blue),
            "a stale recapture callback after teardown must be ignored");
}

void colorPickerFollowsCaptureSessionLifetime() {
    ScreenshotCaptureState state;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligentSelection;
    CaptureRuntime runtime;
    runtime.seedActiveDisplayOnPrepare = true;
    auto workflow = makeWorkflow(state, displays, geometry, interaction, selection,
                                 intelligentSelection, runtime);
    workflow.startCapture(ScreenshotCaptureWorkflow::StartMode::ExternalDrag,
                          ScreenshotCaptureWorkflow::ToolbarPreparation::OnDemand);
    require(runtime.colorPickerAlive && runtime.createColorPickerCalls == 1 &&
                runtime.prepareColorPickerCalls == 1 && runtime.prewarmToolbarSurfaceCalls == 0,
            "every capture must create and prepare a picker independently of the toolbar");
    const auto index = [&](const char* operation) {
        return runtime.operations.indexOf(QString::fromLatin1(operation));
    };
    require(index("create-picker") < index("prepare-overlays") &&
                index("capture-dispatched") < index("prepare-picker"),
            "picker creation must precede preparation and native preparation must follow dispatch");
    if (index("show-overlays") >= 0) {
        require(index("prepare-picker") < index("show-overlays"),
                "the picker must be prepared before overlay presentation");
    }
    workflow.startCapture();
    require(runtime.createColorPickerCalls == 2 && runtime.releaseColorPickerCalls == 1 &&
                runtime.colorPickerAlive,
            "restarting must release the previous picker and create a replacement");
    workflow.cancelCapture();
    require(!runtime.colorPickerAlive && runtime.releaseColorPickerCalls == 2,
            "cancellation must release the session picker");
    workflow.prewarmResources();
    require(runtime.createColorPickerCalls == 2 && !runtime.colorPickerAlive,
            "cleanup and idle prewarm must leave the picker absent");
    workflow.startCapture();
    ScreenshotCaptureResult failed;
    failed.requestId = runtime.lastCaptureRequest.requestId;
    failed.purpose = ScreenshotCapturePurpose::Initial;
    runtime.deliverResult(failed);
    require(!runtime.colorPickerAlive && runtime.releaseColorPickerCalls == 3,
            "asynchronous capture failure must release the picker");
    const int preparations = runtime.prepareColorPickerCalls;
    runtime.deliverResult(failed);
    require(runtime.createColorPickerCalls == 3 && runtime.prepareColorPickerCalls == preparations,
            "stale capture results must not recreate or prepare the picker");
}

void invocationSnapshotIsSharedUntilInput() {
    ScreenshotCaptureState state;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    CaptureRuntime runtime;
    runtime.seedActiveDisplayOnPrepare = false;
    runtime.acceptSelectorHitTest = true;
    int cursorReads = 0;
    QPoint liveCursor(17, 11);
    ScreenshotCaptureWorkflowContext context{state,       runtime,   geometry,    displays,
                                             interaction, selection, intelligent, {}};
    context.cursorPosition = [&] {
        ++cursorReads;
        return liveCursor;
    };
    ScreenshotCaptureWorkflow workflow(context);
    workflow.startCapture();
    require(cursorReads == 1 && runtime.startWorkflowRefreshCalls == 0 && geometry.isEmpty(),
            "startup must sample invocation once and wait for native layout before mapping or "
            "selection");
    liveCursor = QPoint(50, 40);
    require(displays.startup->suppressesInput() &&
                displays.startup->phase == ScreenshotStartupContext::Phase::Preparing,
            "normal startup must reject input before first reveal");
    displays.startup->resumeLiveInput();
    require(displays.startup->phase == ScreenshotStartupContext::Phase::Preparing,
            "resuming before reveal must not release the invocation anchor");
    runtime.deliverLayout();
    require(
        runtime.queriedPoint == QPoint(17, 11) && cursorReads == 1,
        "initial selector query must use the invocation position despite later cursor movement");
    const auto immutableLayout = displays.startup->displays;
    require(immutableLayout && displays.startup->layoutGeneration == state.sessionId,
            "native geometry must be retained with the session generation");
    const QRect originalCanvas = displays.displayAt(0).canvasRect;
    auto frame = displays.displayAt(0);
    frame.image = QImage(64, 48, QImage::Format_RGB32);
    frame.image.fill(Qt::red);
    runtime.deliverResult(successfulResult(state.sessionId, frame));
    require(runtime.showOverlayCalls == 0, "frames must wait for the invocation selection result");
    workflow.handleInitialSmartSelectionResolved(state.sessionId);
    require(runtime.showOverlayCalls == 1 && displays.startup->anchored() &&
                displays.anchoredCursorPosition() == std::optional<QPoint>(QPoint(17, 11)),
            "first reveal must retain the invocation cursor context");
    require(displays.startup->displays == immutableLayout &&
                displays.displayAt(0).canvasRect == originalCanvas &&
                runtime.prepareColorPickerCalls == 1,
            "image attachment must preserve geometry and picker preparation");
    require(displays.startup->suppressesInput() && displays.startup->anchored(),
            "reveal must keep the invocation anchor until real input");
    displays.startup->resumeLiveInput();
    require(!displays.startup->anchored() && !displays.anchoredCursorPosition(),
            "the next real input must release the invocation anchor");
    workflow.cancelCapture();
    require(!displays.startup->displays && !displays.anchoredCursorPosition(),
            "cancel must release geometry and cursor state");
}

void startupMapsMixedDpiAndRejectsChangedLayouts() {
    for (const QPoint invocation : {QPoint(-1, 10), QPoint(0, 10), QPoint(400, 200)}) {
        ScreenshotCaptureState state;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligent;
        CaptureRuntime runtime;
        runtime.seedActiveDisplayOnPrepare = false;
        ScreenshotCaptureWorkflowContext context{state,       runtime,   geometry,    displays,
                                                 interaction, selection, intelligent, {}};
        context.cursorPosition = [=] { return invocation; };
        ScreenshotCaptureWorkflow workflow(context);
        workflow.startCapture();
        CapturedDisplayModel left;
        left.name = QStringLiteral("Left");
        left.stableId = QStringLiteral("left");
        left.logicalRect = QRect(-100, 0, 100, 100);
        left.physicalRect = QRect(-100, 0, 200, 200);
        left.active = true;
        CapturedDisplayModel right;
        right.name = QStringLiteral("Right");
        right.stableId = QStringLiteral("right");
        right.logicalRect = QRect(0, 0, 100, 100);
        right.physicalRect = right.logicalRect;
        right.active = true;
        right.primary = true;
        displays.clear();
        displays.appendDisplay(left);
        displays.appendDisplay(right);
        displays.startup->qtDisplays = {left, right};
        displays.startup->qtDisplaySlots = {0, 1};
        runtime.eventSink->handleLayoutReady({state.sessionId, state.sessionId, {right, left}});
        require(displays.startup->displays && !geometry.isEmpty(),
                "reordered native geometry must match saved screens");
        const QPoint expected = invocation.x() < 0    ? QPoint(98, 20)
                                : invocation.x() == 0 ? QPoint(0, 10)
                                                      : QPoint(99, 99);
        require(displays.startup->physicalPosition == expected,
                "DPI mapping, half-open edges, and gap clamping must agree");
        auto frame = left;
        frame.physicalRect.setWidth(201);
        frame.image = QImage(201, 200, QImage::Format_RGB32);
        auto other = right;
        other.image = QImage(100, 100, QImage::Format_RGB32);
        ScreenshotCaptureResult result;
        result.requestId = state.sessionId;
        result.succeeded = true;
        result.displays = {frame, other};
        runtime.deliverResult(result);
        require(!state.captureInProgress && runtime.showOverlayCalls == 0 &&
                    runtime.refreshLayoutCalls == 1,
                "changed capture geometry must cancel without revealing mismatched pixels");
    }
}

void topologyInvalidatesEveryStartupStage() {
    for (int stage = 0; stage < 3; ++stage) {
        ScreenshotCaptureState state;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligent;
        CaptureRuntime runtime;
        runtime.seedActiveDisplayOnPrepare = false;
        runtime.acceptSelectorHitTest = true;
        auto workflow =
            makeWorkflow(state, displays, geometry, interaction, selection, intelligent, runtime);
        workflow.startCapture();
        const quint64 oldSession = state.sessionId;
        if (stage > 0)
            runtime.deliverLayout();
        if (stage > 1) {
            auto frame = displays.displayAt(0);
            frame.image = QImage(64, 48, QImage::Format_RGB32);
            runtime.deliverResult(successfulResult(oldSession, frame));
        }
        workflow.handleDisplayConfigurationChanged();
        runtime.eventSink->handleLayoutReady({oldSession, oldSession, {}});
        workflow.handleInitialSmartSelectionResolved(oldSession);
        require(state.sessionId != oldSession && runtime.showOverlayCalls == 0 &&
                    !displays.startup->displays,
                "topology changes must invalidate layout, pixels, and selector readiness before "
                "reveal");
        require(runtime.refreshLayoutCalls == 1,
                "invalidated startup must refresh idle layout once");
        workflow.startCapture();
        runtime.deliverLayout();
        require(displays.startup->displays && state.sessionId != oldSession,
                "the next explicit capture must establish a new snapshot");
    }
}

void startupMatchesNativeDisplayIdentityInLogicalCoordinateSpace() {
    ScreenshotCaptureState state;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    CaptureRuntime runtime;
    runtime.seedActiveDisplayOnPrepare = false;
    ScreenshotCaptureWorkflowContext context{state,       runtime,   geometry,    displays,
                                             interaction, selection, intelligent, {}};
    context.cursorPosition = [] { return QPoint(-25, 10); };
    ScreenshotCaptureWorkflow workflow(context);
    workflow.startCapture();
    CapturedDisplayModel qt;
    qt.name = QStringLiteral("Qt display name");
    qt.nativeDisplayId = 42;
    qt.logicalRect = QRect(-100, 0, 100, 80);
    qt.physicalRect = QRect(-100, 0, 200, 160);
    qt.active = true;
    displays.clear();
    displays.appendDisplay(qt);
    displays.startup->qtDisplays = {qt};
    displays.startup->qtDisplaySlots = {0};
    auto native = qt;
    native.name = QStringLiteral("Different native display name");
    native.stableId = QStringLiteral("display:42");
    native.canvasUsesPoints = true;
    native.capturedLogicalRect = qt.logicalRect;
    native.backingScale = 2;
    runtime.eventSink->handleLayoutReady({state.sessionId, state.sessionId, {native}});
    require(displays.startup->displays && displays.startup->nativeDisplayId == 42 &&
                displays.startup->physicalPosition == QPoint(50, 20) &&
                displays.displayAt(0).canvasUsesPoints,
            "logical native geometry must match by display ID and retain backing pixels");
    auto frame = native;
    frame.nativeDisplayId = 43;
    frame.image = QImage(200, 160, QImage::Format_RGB32);
    runtime.deliverResult(successfulResult(state.sessionId, frame));
    require(!displays.startup->displays && runtime.showOverlayCalls == 0,
            "a frame from a different native display must invalidate startup");
}

void startupDisplayIdentityMatchesByNameRectOrNativeId() {
    const auto alwaysCurrent = [](const CapturedDisplayModel&) { return true; };
    const auto display = [](const char* name, const QRect& rect, quint32 id, bool points) {
        CapturedDisplayModel model;
        model.name = QString::fromUtf8(name);
        model.stableId = QString::fromUtf8(name);
        model.physicalRect = rect;
        model.logicalRect = rect;
        model.capturedLogicalRect = rect;
        model.nativeDisplayId = id;
        model.canvasUsesPoints = points;
        model.active = true;
        return model;
    };
    const CapturedDisplayModel left = display("Left", QRect(-100, 0, 200, 200), 0, false);
    const CapturedDisplayModel right = display("Right", QRect(0, 0, 100, 100), 0, false);
    const auto matched = matchStartupDisplays({left, right}, {0, 1}, {right, left}, alwaysCurrent);
    require(matched && matched->size() == 2 && (*matched)[0].slot == 1 &&
                (*matched)[0].identity.sameDeviceName(StartupDisplayIdentity::fromDisplay(right)),
            "reordered monitors must bind by device name");

    for (const QSize pixels : {QSize(2240, 1440), QSize(2560, 1600)}) {
        auto qtScaled = display("Scaled", QRect(QPoint(-2560, -1600), pixels), 0, false);
        qtScaled.logicalRect.setSize(
            QSize(qRound(pixels.width() / 1.5), qRound(pixels.height() / 1.5)));
        qtScaled.logicalToPhysicalScale = 1.5;
        auto nativeScaled = qtScaled;
        nativeScaled.logicalToPhysicalScale = 0;
        nativeScaled.logicalRect = {};
        const auto scaled = matchStartupDisplays({qtScaled}, {0}, {nativeScaled}, alwaysCurrent);
        require(scaled && scaled->first().display.logicalToPhysicalScale == 1.5,
                "native startup matching must retain Qt DPI independently of rounded extents");
        auto resolved = scaled->first().display;
        resolved.active = true;
        ScreenshotDisplaySession displays;
        displays.appendDisplay(resolved);
        ScreenshotGeometryMapper mapper;
        const QPoint logical = resolved.logicalRect.topLeft() + QPoint(1200, 800);
        const QPoint expected = resolved.physicalRect.topLeft() + QPoint(1800, 1200);
        require(mapper.physicalPositionForLogicalPoint(displays, logical) == expected,
                "startup cursor must use exact DPI before canvas geometry is normalized");
        mapper.rebuild(displays);
        require(mapper.physicalPositionForLogicalPoint(displays, logical) == expected,
                "normalizing the canvas must preserve startup cursor pixels");
    }

    CapturedDisplayModel renamed = left;
    renamed.name = QStringLiteral("Other");
    renamed.stableId = QStringLiteral("native-left");
    const auto byRect = matchStartupDisplays({left}, {0}, {renamed}, alwaysCurrent);
    require(
        byRect && byRect->size() == 1 && byRect->first().slot == 0 &&
            byRect->first().identity.samePhysicalRect(StartupDisplayIdentity::fromDisplay(left)),
        "Windows monitors with different names must bind by physical rect");

    const CapturedDisplayModel qt = display("Qt", QRect(0, 0, 200, 160), 42, false);
    CapturedDisplayModel native = display("Native", qt.physicalRect, 42, true);
    native.stableId = QStringLiteral("display:42");
    const auto byId = matchStartupDisplays({qt}, {3}, {native}, alwaysCurrent);
    require(byId && byId->size() == 1 && byId->first().slot == 3 &&
                byId->first().display.geometryResolved && byId->first().identity.usesNativeId,
            "logical displays must bind by native display id");

    CapturedDisplayModel wrongId = native;
    wrongId.nativeDisplayId = 43;
    wrongId.stableId = QStringLiteral("display:43");
    require(!matchStartupDisplays({qt}, {3}, {wrongId}, alwaysCurrent),
            "a different native display id must not fall through to the same rect");

    CapturedDisplayModel duplicate = left;
    duplicate.stableId = QStringLiteral("duplicate");
    require(!matchStartupDisplays({left, left}, {0, 1}, {duplicate}, alwaysCurrent),
            "two Qt displays with the same device name are ambiguous");
}

void injectedLayoutRefreshDoesNotFallBackToEnumeration() {
    class Service final : public ScreenshotSelectorServicePort {
      public:
        int enumerated = 0;
        bool ready() const override {
            return false;
        }
        bool refreshInFlight() const override {
            return false;
        }
        bool startRefresh(const QVector<std::uintptr_t>&) override {
            ++enumerated;
            return true;
        }
        bool requestHitTest(const QPoint&, ScreenshotSelectorHitTestMode) override {
            return false;
        }
    } service;
    require(!service.startRefreshWithDisplays({}, {}),
            "a layout refresh with no displays must fail");
    require(service.enumerated == 0, "failed layout refresh must not enumerate monitors");
    require(service.startRefresh({}), "explicit refresh may enumerate");
    require(service.enumerated == 1, "explicit refresh must be the enumeration path");
    require(
        !service.requestHitTestOnDisplay(QPoint(1, 1), ScreenshotSelectorHitTestMode::Window, 7),
        "a hit test with a display id must not drop that id");
}

void customCaptureDoesNotWaitForSelector() {
    for (auto type : {ScreenshotRegionType::Polyline, ScreenshotRegionType::Curve,
                      ScreenshotRegionType::Freehand}) {
        ScreenshotCaptureState state;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotInteractionState interaction;
        ScreenshotSelectionModel selection;
        selection.setRegionType(type);
        ScreenshotIntelligentSelectionModel intelligent;
        CaptureRuntime runtime;
        runtime.seedActiveDisplayOnPrepare = true;
        runtime.acceptSelectorHitTest = true;
        auto workflow =
            makeWorkflow(state, displays, geometry, interaction, selection, intelligent, runtime);
        workflow.startCapture();
        CapturedDisplayModel snapshot;
        snapshot.stableId = QStringLiteral("primary");
        snapshot.physicalRect = QRect(0, 0, 64, 48);
        snapshot.logicalRect = snapshot.physicalRect;
        snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGB32);
        snapshot.image.fill(Qt::blue);
        runtime.deliverResult(successfulResult(state.sessionId, snapshot));
        require(runtime.showOverlayCalls == 1 && runtime.startWorkflowRefreshCalls == 0 &&
                    interaction.manualSelecting() && intelligent.currentSelection().isEmpty(),
                "custom capture reveals immediately without selector readiness or highlighting");
    }
}

void silentCaptureSuppressesAllPresentationAndRestoresVisibleMode() {
    ScreenshotCaptureState state;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    ScreenshotIntelligentSelectionModel intelligent;
    CaptureRuntime runtime;
    runtime.seedActiveDisplayOnPrepare = true;
    auto workflow =
        makeWorkflow(state, displays, geometry, interaction, selection, intelligent, runtime);
    workflow.startCapture(ScreenshotCaptureWorkflow::StartMode::Normal,
                          ScreenshotCaptureWorkflow::ToolbarPreparation::OnDemand,
                          ScreenshotCaptureWorkflow::ToolbarVisibility::Suppressed,
                          ScreenshotCaptureWorkflow::PresentationMode::Silent);
    CapturedDisplayModel snapshot;
    snapshot.stableId = QStringLiteral("primary");
    snapshot.physicalRect = QRect(0, 0, 64, 48);
    snapshot.logicalRect = snapshot.physicalRect;
    snapshot.image = QImage(snapshot.physicalRect.size(), QImage::Format_RGB32);
    snapshot.image.fill(Qt::blue);
    runtime.deliverResult(successfulResult(state.sessionId, snapshot));
    require(
        state.presentationSuppressed && runtime.showOverlayCalls == 0 &&
            runtime.createColorPickerCalls == 0 && runtime.startWorkflowRefreshCalls == 0 &&
            runtime.prewarmToolbarSurfaceCalls == 0,
        "silent capture must acquire images without exposing overlay, picker, toolbar or selector");
    require(!geometry.isEmpty(), "silent capture retains the native display session");
    workflow.startCapture();
    require(!state.presentationSuppressed && runtime.createColorPickerCalls == 1,
            "normal capture after a silent session restores presentation");
}

int main() {
    silentCaptureSuppressesAllPresentationAndRestoresVisibleMode();
    confirmedSelectionPreservesRegionTypeInToolbarPresentation();
    customCaptureDoesNotWaitForSelector();
    startupDisplayIdentityMatchesByNameRectOrNativeId();
    injectedLayoutRefreshDoesNotFallBackToEnumeration();
    startupMatchesNativeDisplayIdentityInLogicalCoordinateSpace();
    invocationSnapshotIsSharedUntilInput();
    startupMapsMixedDpiAndRejectsChangedLayouts();
    topologyInvalidatesEveryStartupStage();
    colorPickerFollowsCaptureSessionLifetime();
    recapturePreservesEditingStateAndRollsBackFailures();
    toolbarVisibilityIsIndependentOfInputAndPreparation();
    noToolbarCaptureWaitsForUserSelection();
    normalCaptureRestoresToolbarAfterSuppressedCapture();
    externalDragDisplayChangesInvalidatePendingCapture();
    globalDragCoordinatesStayPhysicalAcrossDifferentDisplayScales();
    externalDragBypassesSelectorAndPreparesBeforeReveal();
    initialCaptureSnapshotsScreenshotSettings();
    captureRestoresSelectionPreferencesAfterReset();
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
    phasedSelectionPreservesUserIntent();
    phasedWorkflowOnlySignalsInitialReadinessOnce();
    captureSessionsApplyTheCurrentSmartSelectionSetting();
    return 0;
}
