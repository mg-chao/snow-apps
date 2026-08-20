#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOW_H

#include "snow_shot/presentation/screenshotcaptureworkflowports.h"
#include "snow_shot/presentation/screenshottypes.h"

#include <QVector>

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
    std::function<void(quint64)> deferOverlayState;
    std::function<void()> prepareInitialOverlayState;
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
    std::function<void(std::optional<ScreenshotWindowCaptureFrame>)> focusedWindowCaptured =
        [](std::optional<ScreenshotWindowCaptureFrame>) {};
};

enum class ScreenshotCapturePresentationMode {
    Overlay,
    Silent,
};

class ScreenshotCaptureWorkflow final : private ScreenshotCaptureWorkerEventSink {
  public:
    explicit ScreenshotCaptureWorkflow(ScreenshotCaptureWorkflowContext context);
    ~ScreenshotCaptureWorkflow() override;

    void prewarmResources();
    void startCapture(ScreenshotCapturePresentationMode presentationMode =
                          ScreenshotCapturePresentationMode::Overlay,
                      quintptr focusedWindowHandle = 0);
    void cancelCapture();
    [[nodiscard]] bool handleInitialSmartSelectionResult(
        quint64 sessionId, const QPoint& physicalPoint, bool ok,
        const QVector<QRectF>& physicalHitRects);

    void destroyDisplayPool();
    void destroyUiSelectorService();
    void shutdownCaptureWorker();
    void releaseIdleResources(quint64 sessionId);
    void handleDisplayConfigurationChanged();

  private:
    void clearCapturePresentationReadiness();
    void resetCaptureModels();
    void clearDisplays();
    void cleanupActiveSessionForRestart();
    void releaseResourcesForExternalInvalidation();
    void beginCapturePreparation(quint64 sessionId);
    void finishCapturePreparation(const ScreenshotCaptureResult& result);
    void showCapturePresentationWhenReady(quint64 sessionId);
    void beginInitialSmartSelection(quint64 sessionId);
    void resolveInitialSmartSelection(quint64 sessionId, const QPoint& physicalPoint, bool ok,
                                      const QVector<QRectF>& physicalHitRects);
    void retryInitialSmartSelection(quint64 sessionId);
    [[nodiscard]] bool initialSelectorGeometryMatchesCapture() const;
    void handleCapturePrepared(quint64 requestId, bool ok) override;
    void handleCaptureFinished(const ScreenshotCaptureResult& result) override;
    void prewarmOverlayPool();
    void initializeIdleResources(quint64 requestId);
    void resetCanvasRuntimeState();

    ScreenshotCaptureWorkflowContext m_context;
    ScreenshotCaptureState& m_state;
    quint64 m_capturedPresentationSessionId = 0;
    quint64 m_initialSmartSelectionPendingSessionId = 0;
    quint64 m_initialSmartSelectionResolvedSessionId = 0;
    quint64 m_visiblePresentationSessionId = 0;
    QPoint m_initialSmartSelectionPoint;
    QVector<QRectF> m_initialSmartSelectionPhysicalHitRects;
    QVector<QRect> m_preCapturePhysicalRects;
    bool m_initialSmartSelectionSucceeded = false;
    ScreenshotCapturePresentationMode m_presentationMode =
        ScreenshotCapturePresentationMode::Overlay;
    quintptr m_focusedWindowHandle = 0;
    bool m_captureModelsClean = false;
    bool m_canvasRuntimeClean = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOW_H
