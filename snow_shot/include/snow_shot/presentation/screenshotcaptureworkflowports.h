#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOWPORTS_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QPoint>
#include <QVector>

#include <cstdint>

class ScreenshotDisplaySession;

class ScreenshotCaptureWorkerEventSink {
  public:
    virtual ~ScreenshotCaptureWorkerEventSink() = default;

    virtual void handleCapturePrepared(quint64 requestId, bool ok) = 0;
    virtual void handleCaptureFinished(const ScreenshotCaptureResult& result) = 0;
    virtual void handleLayoutRefreshed(quint64 requestId, bool ok) = 0;
};

class ScreenshotCaptureRuntimePort {
  public:
    virtual ~ScreenshotCaptureRuntimePort() = default;

    virtual void setEventSink(ScreenshotCaptureWorkerEventSink* sink) = 0;
    [[nodiscard]] virtual bool captureWorkerCreated() const = 0;
    [[nodiscard]] virtual bool hasCaptureWorker() const = 0;
    virtual void ensureCaptureWorker() = 0;
    virtual void prepareAsync(quint64 requestId) = 0;
    virtual void refreshLayoutAsync(quint64 requestId) = 0;
    virtual void captureAsync(const ScreenshotCaptureRequest& request) = 0;
    virtual void cancelActiveCapture() = 0;
    virtual void shutdownCaptureWorker() = 0;

    [[nodiscard]] virtual bool selectorReady() const = 0;
    [[nodiscard]] virtual bool selectorRefreshInFlight() const = 0;
    [[nodiscard]] virtual bool selectorHitTestInFlight() const = 0;
    virtual void releaseSelectorCache() = 0;
    virtual void resetHitTestState() = 0;
    virtual void destroySelectorService() = 0;
    virtual void startWorkflowRefresh() = 0;
    virtual void clearSelectorSelection() = 0;
    [[nodiscard]] virtual bool updateSelectorSelectionAt(const QPoint& physicalPoint) = 0;

    virtual void prewarmDisplayPool(ScreenshotDisplaySession& displaySession, int displayCount) = 0;
    virtual void ensureToolbar() = 0;
    virtual void prewarmToolbar() = 0;
    virtual void prewarmOverlayTransientUi(ScreenshotDisplaySession& displaySession) = 0;
    virtual void clearOverlayCanvases(const ScreenshotDisplaySession& displaySession) const = 0;
    virtual void clearDisplays(ScreenshotDisplaySession& displaySession) = 0;
    virtual void destroyDisplayPool(ScreenshotDisplaySession& displaySession) = 0;
    virtual void resetForNewCapture(ScreenshotDisplaySession& displaySession) = 0;
    virtual void prepareDisplayModels(ScreenshotDisplaySession& displaySession) = 0;
    virtual void applyDisplayModels(ScreenshotDisplaySession& displaySession) = 0;
    [[nodiscard]] virtual bool
    preparePreCaptureOverlayWindows(ScreenshotDisplaySession& displaySession) = 0;
    virtual void showOverlayWindows(const ScreenshotDisplaySession& displaySession,
                                    ScreenshotOverlayShowMode mode) = 0;
    virtual void hideOverlayWindows(const ScreenshotDisplaySession& displaySession) = 0;

    [[nodiscard]] virtual bool clearDocumentPreservingViewports() = 0;
    [[nodiscard]] virtual bool resetCanvasRuntime() = 0;
    virtual void resetColorPicker() = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTUREWORKFLOWPORTS_H
