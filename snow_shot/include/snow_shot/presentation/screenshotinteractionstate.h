#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTINTERACTIONSTATE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTINTERACTIONSTATE_H

#include "snow_shot/presentation/screenshotselectiongeometry.h"

enum class ScreenshotActiveTool {
    Move,
    Select,
    Shape,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    Highlight = RectangleHighlight,
    PenHighlight,
    Eraser,
    RectangleFilter,
    Filter = RectangleFilter,
    Watermark,
    Text,
    SerialNumber,
    Ocr,
    Table,
    Qr,
    PenFilter,
    Spotlight,
};

enum class ScreenshotCaptureMode {
    Inactive,
    IntelligentSelecting,
    ManualSelecting,
    Editing,
    MovingSelection,
    ScrollingCapture,
};

class ScreenshotInteractionState final {
  public:
    void reset();
    void beginCapture();
    void enterOverlayVisible(bool selectorReady);
    void setMoveTool(bool hasSelection, bool selectorReady);
    void setCanvasTool(ScreenshotActiveTool tool);
    void setOcrTool();
    void setTableTool();
    void setQrTool();
    void confirmSelection();
    void applySelectionParams();
    void enterScrollingCapture();
    void returnToSelectionMode(bool selectorReady);
    [[nodiscard]] bool enterSelectionDrag(ScreenshotSelectionDragMode dragMode);
    void finishDrag();
    void cancelDrag();

    [[nodiscard]] ScreenshotActiveTool activeTool() const;
    [[nodiscard]] ScreenshotCaptureMode mode() const;
    [[nodiscard]] ScreenshotSelectionDragMode dragMode() const;
    [[nodiscard]] bool dragging() const;
    [[nodiscard]] bool inactive() const;
    [[nodiscard]] bool moveToolActive() const;
    [[nodiscard]] bool intelligentSelecting() const;
    [[nodiscard]] bool manualSelecting() const;
    [[nodiscard]] bool marqueeSelecting() const;
    [[nodiscard]] bool modifyingSelection() const;
    [[nodiscard]] bool movingSelection() const;
    [[nodiscard]] bool editing() const;
    [[nodiscard]] bool scrollingCapture() const;
    [[nodiscard]] bool selecting() const;
    [[nodiscard]] bool cursorMovementEnabled() const;
    [[nodiscard]] bool selectionToolbarMode() const;
    [[nodiscard]] bool canResizeSelection() const;
    [[nodiscard]] bool selectionHandlesVisible() const;

  private:
    ScreenshotActiveTool m_activeTool = ScreenshotActiveTool::Move;
    ScreenshotCaptureMode m_mode = ScreenshotCaptureMode::Inactive;
    ScreenshotSelectionDragMode m_dragMode = ScreenshotSelectionDragMode::None;
    bool m_dragging = false;
    bool m_recognitionSelectionActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTINTERACTIONSTATE_H
