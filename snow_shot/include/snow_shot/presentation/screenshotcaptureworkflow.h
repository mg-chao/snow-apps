#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOW_H

#include "snow_shot/presentation/screenshotcaptureworkflowports.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshottypes.h"

#include <cstdint>
#include <functional>

struct ScreenshotCaptureState;
class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotIntelligentSelectionModel;
class ScreenshotSelectionModel;

struct ScreenshotCapturePresentationCallbacks {
    std::function<void()> hideToolbar;
    std::function<void()> updateOverlayState;
    std::function<void()> updateColorPicker;
    std::function<void()> capturePresented;
    std::function<void()> beforeCapturePresented = []() {};
};

struct ScreenshotCaptureWorkflowContext {
    ScreenshotCaptureState& state;
    ScreenshotCaptureRuntimePort& runtime;
    ScreenshotGeometryMapper& geometry;
    ScreenshotDisplaySession& displaySession;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotIntelligentSelectionModel& intelligentSelection;
    ScreenshotCapturePresentationCallbacks presentation;
    std::function<void()> captureTerminated = []() {};
    std::function<bool()> smartSelectionEnabled = []() { return true; };
    std::function<void()> refreshCanvasCreationStyles = []() {};
    std::function<void()> restoreSelectionEffects = []() {};
    std::function<bool()> restoreOriginalScreenColors = []() { return true; };
    std::function<bool()> captureCursor = []() { return false; };
    std::function<ScreenshotIntelligentSelectionTarget()> preferredSelectionTarget = []() {
        return ScreenshotIntelligentSelectionTarget::WindowSubElement;
    };
};

class ScreenshotCaptureWorkflow final : private ScreenshotCaptureWorkerEventSink {
  public:
    explicit ScreenshotCaptureWorkflow(ScreenshotCaptureWorkflowContext context);
    ~ScreenshotCaptureWorkflow() override;

    void prewarmResources();
    enum class StartMode { Normal, ExternalDrag, NoToolbar };
    void startCapture(StartMode mode = StartMode::Normal);
    [[nodiscard]] bool suppressCaptureToolbar() const;
    void cancelCapture();
    void cancelCaptureForExport();
    void completeDeferredExportCleanup();
    void handleInitialSmartSelectionResolved(quint64 sessionId);

    void destroyDisplayPool();
    void destroyUiSelectorService();
    void shutdownCaptureWorker();
    void handleDisplayConfigurationChanged();

  private:
    void clearCapturePresentationReadiness();
    void resetCaptureModels();
    void clearDisplays();
    void finishCaptureSession(bool deferExportCleanup = false);
    void cleanupActiveSessionForRestart();
    void beginCapturePreparation(quint64 sessionId);
    [[nodiscard]] bool beginCapturePresentation(quint64 sessionId);
    void prepareOverlayPresentation(quint64 sessionId);
    void finishCapturePreparation(const ScreenshotCaptureResult& result);
    void showCapturePresentationWhenReady(quint64 sessionId);
    void enterOverlaySelectionModeAtCursor();
    void handleCapturePrepared(quint64 requestId, bool ok) override;
    void handleCaptureFinished(const ScreenshotCaptureResult& result) override;
    void handleLayoutRefreshed(quint64 requestId, bool ok) override;
    void prewarmOverlayPool();
    void initializeIdleResources(quint64 requestId);
    void scheduleLayoutRefresh(quint64 refreshId);
    void resetCanvasRuntimeState();
    [[nodiscard]] bool capturePresentationPrepared(quint64 sessionId) const;

    ScreenshotCaptureWorkflowContext m_context;
    ScreenshotCaptureState& m_state;
    quint64 m_preparedPresentationSessionId = 0;
    quint64 m_capturedPresentationSessionId = 0;
    quint64 m_initialSmartSelectionPendingSessionId = 0;
    quint64 m_initialSmartSelectionResolvedSessionId = 0;
    quint64 m_visiblePresentationSessionId = 0;
    bool m_captureModelsClean = false;
    bool m_canvasRuntimeClean = false;
    bool m_deferredExportCleanup = false;
    bool m_layoutRefreshInFlight = false;
    bool m_refreshAfterCapture = false;
    quint64 m_layoutChangeSerial = 0;
    StartMode m_startMode = StartMode::Normal;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOW_H
