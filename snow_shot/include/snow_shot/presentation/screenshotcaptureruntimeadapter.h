#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURERUNTIMEADAPTER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURERUNTIMEADAPTER_H

#include "snow_shot/presentation/screenshotcaptureworkflowports.h"

#include <memory>

class ScreenshotColorPickerController;
class ScreenshotOverlayCoordinator;
class ScreenshotSelectorCoordinator;
class ScreenshotSelectorWorkflow;
class SnowCanvasRuntime;

struct ScreenshotCaptureRuntimeAdapterContext {
    ScreenshotSelectorCoordinator& selectorCoordinator;
    ScreenshotSelectorWorkflow& selectorWorkflow;
    ScreenshotOverlayCoordinator& overlayCoordinator;
    ScreenshotColorPickerController& colorPickerController;
    SnowCanvasRuntime& canvasRuntime;
};

class ScreenshotCaptureCoordinator;

class ScreenshotCaptureRuntimeAdapter final : public ScreenshotCaptureRuntimePort {
  public:
    explicit ScreenshotCaptureRuntimeAdapter(ScreenshotCaptureRuntimeAdapterContext context);
    ~ScreenshotCaptureRuntimeAdapter() override;

    void setEventSink(ScreenshotCaptureWorkerEventSink* sink) override;
    [[nodiscard]] bool captureWorkerCreated() const override;
    [[nodiscard]] bool hasCaptureWorker() const override;
    void ensureCaptureWorker() override;
    void prepareAsync(quint64 requestId) override;
    void refreshLayoutAsync(quint64 requestId) override;
    void captureAsync(const ScreenshotCaptureRequest& request) override;
    void cancelActiveCapture() override;
    void releaseIdleResourcesAsync(quint64 requestId) override;
    void shutdownCaptureWorker() override;

    [[nodiscard]] bool selectorReady() const override;
    [[nodiscard]] bool selectorRefreshInFlight() const override;
    [[nodiscard]] bool selectorHitTestInFlight() const override;
    void releaseSelectorCache() override;
    void resetHitTestState() override;
    void destroySelectorService() override;
    void startWorkflowRefresh() override;
    void clearSelectorSelection() override;
    [[nodiscard]] bool updateSelectorSelectionAt(const QPoint& physicalPoint) override;

    void prewarmDisplayPool(ScreenshotDisplaySession& displaySession, int displayCount) override;
    void ensureToolbar() override;
    void prewarmToolbar() override;
    void prewarmOverlayTransientUi(ScreenshotDisplaySession& displaySession) override;
    void clearOverlayCanvases(const ScreenshotDisplaySession& displaySession) const override;
    void clearDisplays(ScreenshotDisplaySession& displaySession) override;
    void destroyDisplayPool(ScreenshotDisplaySession& displaySession) override;
    void resetForNewCapture(ScreenshotDisplaySession& displaySession) override;
    void prepareDisplayModels(ScreenshotDisplaySession& displaySession) override;
    void applyDisplayModels(ScreenshotDisplaySession& displaySession) override;
    [[nodiscard]] bool
    preparePreCaptureOverlayWindows(ScreenshotDisplaySession& displaySession) override;
    void showOverlayWindows(const ScreenshotDisplaySession& displaySession,
                            ScreenshotOverlayShowMode mode) override;
    void hideOverlayWindows(const ScreenshotDisplaySession& displaySession) override;

    [[nodiscard]] bool clearDocumentPreservingViewports() override;
    [[nodiscard]] bool resetCanvasRuntime() override;
    void resetColorPicker() override;

  private:
    void ensureCaptureCoordinator();

    ScreenshotCaptureRuntimeAdapterContext m_context;
    std::unique_ptr<ScreenshotCaptureCoordinator> m_captureCoordinator;
    ScreenshotCaptureWorkerEventSink* m_captureEventSink = nullptr;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURERUNTIMEADAPTER_H
