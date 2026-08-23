#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H

#include <QObject>
#include <QString>

#include <memory>

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotuipreferences.h"

class ScreenshotController : public QObject {
    Q_OBJECT

  public:
    explicit ScreenshotController(QObject* parent = nullptr);
    ~ScreenshotController() override;

    void setUiPreferences(const ScreenshotUiPreferences& preferences);
    [[nodiscard]] bool hasActiveWork() const;

    // The application keeps this controller alive for tray/shortcut commands, but its lazy
    // implementation owns the capture worker, overlay pool, and canvas runtime. A lifecycle
    // owner can request that idle implementation to be torn down after a visible surface closes.
    // The request is ignored while capture/export/recording work or a presented window is live.
    void requestIdleResourceRelease();
    void cancelIdleResourceRelease();

  public slots:
    void prewarmResources();
    void startCapture();
    void startDelayedCapture(int delaySeconds);
    void captureAndPinSelection();
    void captureAndRecognizeText();
    void captureAndTranslateText();
    void captureAndCopySelection();
    void captureCurrentMonitor();
    void captureFocusedWindow();
    void captureAndStartScreenRecording();
    void startOrStopScreenRecordingAndCopy();
    void editHistoryRecord(const QString& recordId);
    void cancelCapture();
    void copySelectionToClipboard();
    void pinSelectionToScreen();
    void pinClipboardContentToScreen();
    void startScreenRecording();
    void setMoveTool();
    void setSelectTool();
    void setShapeTool();
    void setArrowTool();
    void setLineTool();
    void setFreeDrawTool();
    void setHighlightTool();
    void setPenHighlightTool();
    void setSpotlightTool();
    void setEraserTool();
    void setFilterTool();
    void setRectangleFilterTool();
    void setPenFilterTool();
    void setWatermarkTool();
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig& config);
    void setSpotlightConfigFromToolbar(const SnowCanvasSpotlightConfig& config);
    void setTextTool();
    void setSerialNumberTool();
    void decrementSelectedSerialNumbers();
    void incrementSelectedSerialNumbers();
    void createTextForSelectedSerialNumber();
    SnowCanvasShapeStyle currentRectangleStyle() const;
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style, quint32 properties,
                                  SnowCanvasShapeKind kind);
    void setTextStyleFromToolbar(const SnowCanvasTextStyle& style);
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle& style);
    void adjustSelectionFromToolbar(int minDx, int minDy, int maxDx, int maxDy);
    void setSelectionCornerRadiusFromToolbar(int radius);
    void setSelectionShadowWidthFromToolbar(int shadowWidth);
    void toggleSelectionAspectRatioLockFromToolbar();
    void openSelectionResizeModalFromToolbar();
    void repositionToolbarForContentChange();
    void hideColorPickersForScreenshotUi();

  signals:
    void showApplicationInterfaceRequested();

  private:
    struct Impl;

    Impl& ensureImpl();
    Impl* activeCaptureImpl() noexcept;
    const Impl* activeCaptureImpl() const noexcept;
    quint64 nextOperationGeneration();
    void retryIdleImplementationRelease();
    void scheduleIdleImplementationRelease(Impl* implementation);

    std::unique_ptr<Impl> m_impl;
    quint64 m_operationGeneration = 0;
    quint64 m_idleReleaseGeneration = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCONTROLLER_H
