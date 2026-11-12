#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINPUTHANDLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINPUTHANDLER_H

#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotselectionaspectratio.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotselectioneffectgeometry.h"
#include "snow_shot/presentation/screenshotwheelinput.h"

#include "snow_shot/image/screenshotregiongeometry.h"
#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"
#include <QCursor>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <Qt>

#include <functional>
#include <optional>

class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotIntelligentSelectionModel;
class ScreenshotOverlayWindow;
class ScreenshotSelectionModel;
class QWidget;
class SnowCanvasWidget;
struct ScreenshotCaptureState;
struct ScreenshotSelectionParams;
enum class ScreenshotActiveTool;

// Only a border resize retains the recognition window that owns the pointer gesture.
enum class ScreenshotToolActivationReason { Command, SelectionResize, Reselection };

// Region replacements complete the action that opened this capture. An explicit toolbar
// command takes over that action and allows its own editing UI before dispatch.
enum class ScreenshotSelectionIntent { PreserveLaunchAction, SupersedeWithCommand };

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
    std::function<bool(int delta)> stepBrushFilterStrokeWidth = [](int) { return false; };
    std::function<bool(int delta)> stepWatermarkFontSize = [](int) { return false; };
    std::function<void()> copySelectionToClipboard = []() {};
    std::function<bool()> localShortcutInputAllowed = []() { return true; };
    // Toolbar commands dispatch through the palette's button action path.
    std::function<bool(const QString& actionId)> activateScreenshotShortcut = [](const QString&) {
        return false;
    };
    std::function<bool(const QString& toolId)> activateDrawingShortcut = [](const QString&) {
        return false;
    };
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
    std::function<bool()> toggleColorPickerCoordinateMode = []() { return false; };
    std::function<bool(const QPoint& direction)> moveCursorOnePixel = [](const QPoint&) {
        return false;
    };

    // Called after committing a selection and applying any explicit or restored tool,
    // before the handler presents editing UI. Activate pending capture actions or
    // the remembered tool here; commands may retire the session.
    std::function<void()> selectionConfirmed = []() {};

    // Applies the next older screenshot history selection to the current image.
    std::function<bool()> selectPreviousSelection = []() { return false; };

    // The reason determines whether an existing recognition input surface may survive.
    std::function<bool(ScreenshotActiveTool tool, ScreenshotToolActivationReason reason)>
        activateTool = [](ScreenshotActiveTool, ScreenshotToolActivationReason) { return false; };

    // Canvas color sampler callbacks.
    std::function<void()> cancelCanvasColorSampling = []() {};
    std::function<bool(ScreenshotOverlayWindow* overlay, const QPointF& localPosition)>
        sampleCanvasColor = [](ScreenshotOverlayWindow*, const QPointF&) { return false; };

    // Scrolling capture uses a pass-through hole over the selected area. A border
    // resize temporarily removes that hole and starts the capture again once the
    // gesture has committed.
    std::function<void()> pauseScrollingCapture = []() {};
    std::function<void()> resumeScrollingCapture = []() {};

    // Confirmed-session tool shortcuts follow toolbar visibility. Selection-stage
    // shortcuts prepare the current region before using the same toolbar commands.
    std::function<bool()> mainToolbarVisible = []() { return true; };

    std::function<void(ScreenshotOverlayWindow* overlay, const QPointF& localPosition)>
        previewCanvasColor = [](ScreenshotOverlayWindow*, const QPointF&) {};

    // Keep new actions at the end so positional test and application initializers remain valid.
    std::function<bool()> physicalCursorMovementAvailable = []() { return false; };
    std::function<bool()> recaptureAvailable = []() { return false; };
    std::function<bool()> cancelCaptureViaShortcut = []() { return false; };
    std::function<void(const QPoint&, quint32)> requestUiSelectorHitTestOnDisplay;
    std::function<bool(const QString&)> canActivateScreenshotShortcut = [](const QString&) {
        return true;
    };
    std::function<bool(const QString&)> canActivateDrawingShortcut = [](const QString&) {
        return true;
    };
    // Apply intent before changing the region or dispatching the command. This callback
    // owns launch-action and toolbar policy; the handler owns synchronous confirmation.
    std::function<void(ScreenshotSelectionIntent)> prepareSelection =
        [](ScreenshotSelectionIntent) {};
    std::function<bool()> toggleGuidesForCurrentSession = []() { return false; };
    std::function<bool()> cursorVisibilityAvailable = [] { return false; };
    std::function<bool()> toggleCursorVisibility = [] { return false; };
    std::function<void(ScreenshotOverlayWindow*, ScreenshotSelectionEffectHandle)> setEffectCursor =
        [](ScreenshotOverlayWindow*, ScreenshotSelectionEffectHandle) {};
    std::function<void(ScreenshotSelectionEffectHandle, int)> previewSelectionEffect = {};
    std::function<void()> commitSelectionEffects = [] {};
    std::function<SnowCanvasWidget*(const ScreenshotOverlayWindow*)> effectCanvas =
        [](const ScreenshotOverlayWindow*) { return nullptr; };
    std::function<void(ScreenshotSelectionAspectRatioPreset, bool)>
        persistSelectionAspectRatioPreference = [](ScreenshotSelectionAspectRatioPreset, bool) {};
    std::function<QPoint()> currentLogicalCursorPosition = [] { return QCursor::pos(); };
    // Only continuous drag geometry uses this request; gesture boundaries and commands stay direct.
    std::function<void()> requestSelectionDragPresentation = {};
    std::function<void(int minDx, int minDy, int maxDx, int maxDy)> adjustSelectionFromToolbar =
        [](int, int, int, int) {};
    std::function<bool()> selectNextPreviousSelection = [] { return false; };
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
    ~ScreenshotOverlayInputHandler();
    // Selection replacement must wait for the current input owner, including the gaps
    // between draft clicks and an external gesture whose first frame has not arrived yet.
    [[nodiscard]] bool selectionTransactionActive() const;
    bool effectDragActive() const;
    bool cancelEffectDrag();
    void leaveEffectEditors();
    bool handleEffectDoubleClick(ScreenshotOverlayWindow* overlay, const QPointF& position);
    void setExternalDragActive(bool active) {
        if (m_externalDragActive && !active) {
            resetTransientShortcuts();
        }
        if (active && !m_externalDragActive && m_context.actions.pauseIntelligentSelection) {
            m_context.actions.pauseIntelligentSelection();
        }
        m_externalDragActive = active;
    }

    [[nodiscard]] bool externalDragActive() const {
        return m_externalDragActive;
    }
    [[nodiscard]] bool acceptInput(bool genuine = true);
    void beginExternalSelectionDrag(const QPointF& canvasPosition);
    void beginExternalSelectionDrag(const QPointF& start, const QPointF& latestEnd);
    void updateExternalSelectionDrag(const QPointF& canvasPosition);

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
    void updateRegionSnapModifiers(Qt::KeyboardModifiers modifiers, bool recompute = true);
    void clearRegionSnapping();
    void completeRightClickCancellation();
    [[nodiscard]] ScreenshotOverlayRightClickResult
    handleRightClick(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    void handleUnhandledLeftDoubleClick();
    bool handleRegionDoubleClick(ScreenshotOverlayWindow* overlay, const QPointF& position);
    void setRegionType(ScreenshotRegionType type);
    bool cycleRegionType(bool reverse);
    bool removeRegionVertex();
    bool customRegionInputActive() const;
    void handleUnhandledMiddleClick(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    [[nodiscard]] bool handleWheel(ScreenshotOverlayWindow* overlay, const QWheelEvent& event);
    [[nodiscard]] bool shouldBlockUnhandledKeyInput() const;
    [[nodiscard]] bool activateMoveEntireSelectionShortcut();
    [[nodiscard]] bool activateKeepSelectionAspectRatioShortcut(bool cycleColorFormatIfUnused);
    [[nodiscard]] bool canActivateSelectionAspectRatioSnapShortcut() const;
    [[nodiscard]] bool activateSelectionAspectRatioSnapShortcut();
    bool releaseMoveEntireSelectionShortcut();
    bool releaseKeepSelectionAspectRatioShortcut();
    bool releaseSelectionAspectRatioSnapShortcut();
    void cancelKeepSelectionAspectRatioShortcut();
    void cancelSelectionAspectRatioSnapShortcut();
    [[nodiscard]] bool toggleIntelligentSelectionTargetShortcut();
    void resetTransientShortcuts();
    // Session termination releases construction state even while its draft is still active.
    void endCaptureSession();
    [[nodiscard]] bool canvasColorSamplingActive() const;
    void armCanvasColorSampling();
    void cancelCanvasColorSampling();

  private:
    friend struct ScreenshotSessionInputCleanupTestAccess;

    ScreenshotSelectionEffectLayout effectLayout(const ScreenshotOverlayWindow* overlay) const;
    ScreenshotSelectionEffectHandle effectHandleAt(const ScreenshotOverlayWindow* overlay,
                                                   const QPointF& localPosition) const;
    bool updateEffectHover(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    bool beginEffectDrag(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    void updateEffectDrag(const QPointF& canvasPosition);
    void finishEffectDrag(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    QPointer<QWidget> m_effectMouseGrab;
    bool m_consumeEffectRelease = false;
    void executeConfiguredCompletionAction(const QString& action,
                                           std::optional<QPointF> middleClickPosition);
    void beginSelectionDrag(ScreenshotOverlayWindow* overlay, const QPointF& virtualPosition,
                            ScreenshotSelectionDragMode dragMode,
                            const std::optional<QPointF>& latestEnd = std::nullopt);
    void handleIntelligentSelectionPress(const QPointF& virtualPosition);
    void handleIntelligentSelectionMove(ScreenshotOverlayWindow* overlay,
                                        const QPointF& localPosition,
                                        const QPointF& virtualPosition);
    void handleHoverMove(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    void updateGuideLines(ScreenshotOverlayWindow* overlay, const QPointF& localPosition) const;
    void updateSelectionDrag(const QPointF& virtualPosition);
    void updateSelectionDragState(const QPointF& virtualPosition);
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
    // Restore persisted geometry through the same confirmation lifecycle as a mouse selection.
    [[nodiscard]] bool restorePreviousSelection(const ScreenshotSelectionParams& params);
    [[nodiscard]] bool canSelectCurrentScreen() const;
    // Select the current display, expanding an already confirmed whole display to all displays.
    [[nodiscard]] bool selectCurrentScreen();
    [[nodiscard]] bool canPrepareSelectionForToolbarShortcut() const;
    // Commit the region, activate the command, then present its resulting tool.
    [[nodiscard]] bool activateToolbarShortcutForSelection(const std::function<bool()>& activate);
    void discardReselectionTool();
    void beginRegionOperation(bool subtract);
    bool cancelRegionOperation();
    [[nodiscard]] bool regionOperationActive() const;
    [[nodiscard]] bool multiRegionEditing() const;
    [[nodiscard]] bool canDuplicateLatestRegion() const;
    bool duplicateLatestRegion();
    bool duplicateLatestRegionAt(const QPointF& canvasPosition);

  private:
    enum class AspectRatioSnapShortcutState { Released, Snapping, Unlocking };

    bool confirmSelection(const std::function<void()>& beforePresentation);
    [[nodiscard]] bool canPresentSelection(quint64 sessionId) const;
    [[nodiscard]] QPointF virtualPositionForOverlay(const ScreenshotOverlayWindow* overlay,
                                                    const QPointF& localPosition) const;
    [[nodiscard]] QPoint physicalPositionForCanvasPoint(const QPointF& point) const;
    [[nodiscard]] ScreenshotSelectionDragMode
    dragModeForVirtualPosition(const QPointF& virtualPosition, bool borderOnly) const;
    [[nodiscard]] ScreenshotSelectionDragMode
    dragModeForPosition(const ScreenshotOverlayWindow* overlay, const QPointF& localPosition,
                        bool borderOnly) const;
    [[nodiscard]] bool outsideClickRecreatesSelection() const;
    [[nodiscard]] QRectF selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                              const QPointF& position);
    void restoreToolAfterSelectionResize();
    void restoreToolAfterReselection();
    void restoreScrollingCaptureAfterFailedResize();
    void finishTransientDrag();

    void flushFreehandPoints(bool finish = false);
    void updateRegionDraft(const QPointF& pointer, bool includePointer);
    bool finishRegionDraft();
    QVector<QPointF> m_regionPoints;
    qsizetype m_freehandRawStart = 0;
    SnowCanvasStrokeFilter m_freehandFilter;
    QTimer m_regionPreviewTimer;
    QPointF m_pendingRegionPointer;
    bool m_pendingRegionEdge = false;
    bool m_freehandPressed = false;
    bool m_consumeRegionRelease = false;
    ScreenshotOverlayInputHandlerContext m_context;
    snow_shot::presentation::WheelStepAccumulator m_selectionWheelSteps;
    snow_shot::presentation::WheelStepAccumulator m_selectionResizeWheelSteps;
    snow_shot::presentation::WheelStepAccumulator m_angleWheelSteps;
    QPointer<SnowCanvasWidget> m_angleWheelCanvas;
    QMetaObject::Connection m_angleTargetConnection;
    bool m_externalDragActive = false;
    std::optional<ScreenshotActiveTool> m_toolBeforeSelectionResize;
    // Retained across selection gestures until confirmation or an explicit tool command.
    std::optional<ScreenshotActiveTool> m_toolBeforeReselection;
    bool m_scrollingCaptureSelectionResize = false;
    bool m_moveEntireSelectionShortcut = false;
    bool m_keepSelectionAspectRatioShortcut = false;
    AspectRatioSnapShortcutState m_selectionAspectRatioSnapShortcut =
        AspectRatioSnapShortcutState::Released;
    bool m_snappedDuringSelectionDrag = false;
    bool m_regionSnapCtrl = false;
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
