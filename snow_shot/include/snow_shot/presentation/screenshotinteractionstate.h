#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTINTERACTIONSTATE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTINTERACTIONSTATE_H

#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotselectioneffectgeometry.h"

#include <functional>
#include <optional>

class ScreenshotSelectionModel;

enum class ScreenshotActiveTool {
    Move,
    Select,
    Shape,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    PenHighlight,
    Eraser,
    RectangleFilter,
    Watermark,
    Text,
    SerialNumber,
    Ocr,
    Table,
    Qr,
    PenFilter,
    Spotlight,
    Markdown,
    Html,
    AutoFilter,
    Latex,
    RectangleEraser,
    BrushEraser,
};

[[nodiscard]] inline bool isScreenshotRecognitionTool(ScreenshotActiveTool tool) {
    return tool == ScreenshotActiveTool::Ocr || tool == ScreenshotActiveTool::Table ||
           tool == ScreenshotActiveTool::Latex || tool == ScreenshotActiveTool::Qr ||
           tool == ScreenshotActiveTool::Markdown || tool == ScreenshotActiveTool::Html;
}

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
    struct EffectGesture {
        ScreenshotSelectionEffectHandle handle = ScreenshotSelectionEffectHandle::None;
        QPointF pressPosition;
        int originalValue = 0;
        qreal maximumRadius = 0;
        qreal radiusPerCanvasUnit = 0;
        qreal shadowDragDirection = 1.0;
        std::function<void()> rollback;
    };

    bool beginEffectDrag(EffectGesture gesture);
    void finishEffectDrag();
    bool cancelEffectDrag();
    const std::optional<EffectGesture>& effectGesture() const {
        return m_effectGesture;
    }
    ScreenshotSelectionEffectHandle hoveredEffectHandle() const {
        return m_hoveredEffectHandle;
    }
    void setHoveredEffectHandle(ScreenshotSelectionEffectHandle handle) {
        m_hoveredEffectHandle = handle;
    }
    bool effectEditorsSuppressed() const {
        return m_effectEditorsSuppressed;
    }
    void setEffectEditorsSuppressed(bool suppressed) {
        m_effectEditorsSuppressed = suppressed;
        if (suppressed)
            resetEffectEditors();
    }

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
    [[nodiscard]] bool preselectionActive(const ScreenshotSelectionModel& selection) const;
    [[nodiscard]] bool cursorMovementEnabled() const;
    [[nodiscard]] bool selectionToolbarMode() const;
    [[nodiscard]] bool canResizeSelection() const;
    [[nodiscard]] bool selectionHandlesVisible() const;

  private:
    void resetEffectEditors();
    std::optional<EffectGesture> m_effectGesture;
    ScreenshotSelectionEffectHandle m_hoveredEffectHandle = ScreenshotSelectionEffectHandle::None;
    bool m_effectEditorsSuppressed = false;
    ScreenshotActiveTool m_activeTool = ScreenshotActiveTool::Move;
    ScreenshotCaptureMode m_mode = ScreenshotCaptureMode::Inactive;
    ScreenshotSelectionDragMode m_dragMode = ScreenshotSelectionDragMode::None;
    bool m_dragging = false;
    bool m_marqueeGesture = false;
    bool m_recognitionSelectionActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTINTERACTIONSTATE_H
