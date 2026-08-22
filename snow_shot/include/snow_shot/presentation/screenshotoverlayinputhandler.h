#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINPUTHANDLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINPUTHANDLER_H

#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"

#include <QPoint>
#include <QPointF>
#include <Qt>

#include <functional>
#include <optional>

class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotIntelligentSelectionModel;
class ScreenshotOverlayWindow;
class ScreenshotSelectionModel;
struct ScreenshotCaptureState;
enum class ScreenshotActiveTool;

struct ScreenshotOverlayInputActions {
    std::function<bool(const QPoint& physicalPoint)> returnToIntelligentSelection =
        [](const QPoint&) { return false; };
    std::function<void(const QPoint& physicalPoint)> requestUiSelectorHitTest = [](const QPoint&) {
    };
    std::function<void()> pauseIntelligentSelection = []() {};

    std::function<void(ScreenshotOverlayWindow* overlay, ScreenshotSelectionDragMode dragMode)>
        setOverlayCursor = [](ScreenshotOverlayWindow*, ScreenshotSelectionDragMode) {};
    std::function<void()> hideMainToolbar = []() {};
    std::function<void()> updateOverlayState = []() {};
    std::function<void()> showToolbar = []() {};
    std::function<void()> showSelectionToolbar = []() {};
    std::function<void()> cancelCapture = []() {};
    std::function<bool(int delta)> stepStrokeWidth = [](int) { return false; };
    std::function<bool(int delta)> stepSelectionOpacity = [](int) { return false; };
    std::function<bool(int delta)> stepSpotlightOpacity = [](int) { return false; };
    std::function<bool(int delta)> stepFilterIntensity = [](int) { return false; };
    std::function<bool(int delta)> stepPenFilterStrokeWidth = [](int) { return false; };
    std::function<bool(int delta)> stepWatermarkFontSize = [](int) { return false; };
    std::function<void()> copySelectionToClipboard = []() {};
    std::function<void(const QString& action)> executeConfiguredCompletionAction =
        [](const QString&) {};
    std::function<bool()> localShortcutInputAllowed = []() { return true; };
    std::function<bool()> activateMoveTool = []() { return false; };
    std::function<bool(const QString& toolId)> activateDrawingShortcut =
        [](const QString&) { return false; };
    std::function<bool()> navigateHistoryPrevious = []() { return false; };
    std::function<bool()> navigateHistoryNext = []() { return false; };
    std::function<bool()> returnToCurrentScreenshot = []() { return false; };

    std::function<void(ScreenshotOverlayWindow* overlay, const QPointF& localPosition)>
        updateColorPickerForOverlay = [](ScreenshotOverlayWindow*, const QPointF&) {};
    std::function<void(ScreenshotOverlayWindow* overlay, const QPointF& localPosition)>
        updateGuideLinesForOverlay = [](ScreenshotOverlayWindow*, const QPointF&) {};
    std::function<void(const QPointF& virtualPosition)> updateColorPickerForSelectionDrag =
        [](const QPointF&) {};
    std::function<bool()> copyColorPickerColorToClipboard = []() { return false; };
    std::function<bool()> cycleColorPickerFormat = []() { return false; };
    std::function<bool(snow_shot::platform::PhysicalCursorDirection direction)> moveCursorOnePixel =
        [](snow_shot::platform::PhysicalCursorDirection) { return false; };

    // Called after a valid selection transitions the interaction into editing.
    // This is intentionally separate from showToolbar so callers can schedule
    // a post-selection command (for example, OCR or pinning) without coupling
    // the input handler to a concrete controller.
    std::function<void()> selectionConfirmed = []() {};

    // Applies the persisted selection rectangle from the preceding screenshot.
    // Keep this at the end of the aggregate so existing positional initializers
    // remain source-compatible.
    std::function<bool()> selectPreviousSelection = []() { return false; };

    // Performs the same lasting tool activation as a direct toolbar command.
    std::function<bool(ScreenshotActiveTool tool)> activateToolForSelectionResize =
        [](ScreenshotActiveTool) { return false; };

    // Keep sampler callbacks together to preserve existing positional initializers.
    std::function<void()> cancelCanvasColorSampling = []() {};
    std::function<bool(ScreenshotOverlayWindow* overlay, const QPointF& localPosition)>
        sampleCanvasColor = [](ScreenshotOverlayWindow*, const QPointF&) { return false; };

    // Scrolling capture uses a pass-through hole over the selected area. A border
    // resize temporarily removes that hole and starts the capture again once the
    // gesture has committed.
    std::function<void()> pauseScrollingCapture = []() {};
    std::function<void()> resumeScrollingCapture = []() {};

    // Tool-switch shortcuts mirror the main toolbar's availability. Contextual
    // screenshot shortcuts remain active while the toolbar is temporarily hidden.
    std::function<bool()> mainToolbarVisible = []() { return true; };

    std::function<void(ScreenshotOverlayWindow* overlay, const QPointF& localPosition)>
        previewCanvasColor = [](ScreenshotOverlayWindow*, const QPointF&) {};

    std::function<bool()> activateTextRecognition = []() { return false; };
    std::function<bool()> activateTableRecognition = []() { return false; };
    std::function<bool()> activateQrRecognition = []() { return false; };
    std::function<bool()> startVideoRecording = []() { return false; };
    std::function<bool()> startScrollingScreenshot = []() { return false; };
    std::function<bool()> saveAsFile = []() { return false; };
    std::function<bool()> activateTextTranslation = []() { return false; };
    std::function<bool()> pinSelectionToScreen = []() { return false; };
    std::function<bool()> undo = []() { return false; };
    std::function<bool()> redo = []() { return false; };

    // Keep new actions at the end so positional test and application initializers remain valid.
    std::function<bool()> physicalCursorMovementAvailable = []() { return false; };
};

struct ScreenshotOverlayInputHandlerContext {
    ScreenshotCaptureState& captureState;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotIntelligentSelectionModel& intelligentSelection;
    const ScreenshotGeometryMapper& geometry;
    const ScreenshotDisplaySession& displaySession;
    ScreenshotOverlayInputActions actions;
};

class ScreenshotOverlayInputHandler final {
  public:
    explicit ScreenshotOverlayInputHandler(ScreenshotOverlayInputHandlerContext context);

    void handleMousePress(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    [[nodiscard]] ScreenshotSelectionDragMode
    selectionResizeDragModeAtCanvasPosition(const QPointF& canvasPosition) const;
    [[nodiscard]] bool beginSelectionResizeAtCanvasPosition(const QPointF& canvasPosition);
    void updateSelectionResizeAtCanvasPosition(const QPointF& canvasPosition);
    void finishSelectionResizeAtCanvasPosition(const QPointF& canvasPosition);
    [[nodiscard]] bool shouldHandleMouseEvent(const ScreenshotOverlayWindow* overlay,
                                              const QPointF& localPosition,
                                              bool leftButtonActive) const;
    void handleMouseMove(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    void handleMouseRelease(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    [[nodiscard]] bool handleRightClick(ScreenshotOverlayWindow* overlay,
                                        const QPointF& localPosition);
    void handleUnhandledLeftDoubleClick();
    void handleUnhandledMiddleClick();
    [[nodiscard]] bool handleWheel(ScreenshotOverlayWindow* overlay, const QPointF& localPosition,
                                   const QPoint& angleDelta, const QPoint& pixelDelta);
    [[nodiscard]] bool shouldBlockUnhandledKeyInput() const;
    [[nodiscard]] bool activateMoveEntireSelectionShortcut();
    [[nodiscard]] bool activateKeepSelectionAspectRatioShortcut(
        bool cycleColorFormatIfUnused);
    bool releaseMoveEntireSelectionShortcut();
    bool releaseKeepSelectionAspectRatioShortcut();
    [[nodiscard]] bool toggleIntelligentSelectionTargetShortcut();
    void resetTransientShortcuts();
    [[nodiscard]] bool canvasColorSamplingActive() const;
    void armCanvasColorSampling();
    void cancelCanvasColorSampling();

  private:
    void beginSelectionDrag(ScreenshotOverlayWindow* overlay, const QPointF& virtualPosition,
                            ScreenshotSelectionDragMode dragMode);
    void handleIntelligentSelectionPress(const QPointF& virtualPosition);
    void handleIntelligentSelectionMove(ScreenshotOverlayWindow* overlay,
                                        const QPointF& localPosition,
                                        const QPointF& virtualPosition);
    void handleHoverMove(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    void updateGuideLines(ScreenshotOverlayWindow* overlay, const QPointF& localPosition) const;
    void updateSelectionDrag(const QPointF& virtualPosition);
    void handleIntelligentSelectionRelease(const QPointF& virtualPosition);
    void finishSelectionDrag(ScreenshotOverlayWindow* overlay, const QPointF& localPosition,
                             const QPointF& virtualPosition);
    void requestIntelligentSelectionHitTest(const QPointF& virtualPosition);
    void setIntelligentSelectionIndex(int index);
  public:
    // Confirms the current model selection and invokes selectionConfirmed.
    // This is also used by non-interactive quick actions that select a whole
    // monitor or a focused window after the capture frame arrives.
    void confirmSelection();

  private:
    [[nodiscard]] QPointF virtualPositionForOverlay(const ScreenshotOverlayWindow* overlay,
                                                    const QPointF& localPosition) const;
    [[nodiscard]] QPoint physicalPositionForCanvasPoint(const QPointF& point) const;
    [[nodiscard]] ScreenshotSelectionDragMode
    dragModeForVirtualPosition(const QPointF& virtualPosition, bool borderOnly) const;
    [[nodiscard]] ScreenshotSelectionDragMode
    dragModeForPosition(const ScreenshotOverlayWindow* overlay, const QPointF& localPosition,
                        bool borderOnly) const;
    [[nodiscard]] QRectF selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                              const QPointF& position) const;
    void restoreToolAfterSelectionResize();
    void restoreScrollingCaptureAfterFailedResize();
    void finishTransientDrag();

    ScreenshotOverlayInputHandlerContext m_context;
    std::optional<ScreenshotActiveTool> m_toolBeforeSelectionResize;
    bool m_scrollingCaptureSelectionResize = false;
    bool m_moveEntireSelectionShortcut = false;
    bool m_keepSelectionAspectRatioShortcut = false;
    bool m_aspectShortcutUsedForSelectionDrag = false;
    bool m_cycleColorFormatIfAspectShortcutUnused = false;
    ScreenshotSelectionDragMode m_moveDragModeBeforeShortcut = ScreenshotSelectionDragMode::None;
    // The fixed corner of a marquee is translated along with the rectangle
    // while Space temporarily changes the drag into whole-selection movement.
    QPointF m_marqueeAnchor;
    QPointF m_lastMoveDragPosition;
    bool m_canvasColorSamplingArmed = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINPUTHANDLER_H
