#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"

namespace {
bool recognitionTool(ScreenshotActiveTool tool) {
    return isScreenshotRecognitionTool(tool);
}

bool drawingToolSupportsCursorMovement(ScreenshotActiveTool tool) {
    return tool != ScreenshotActiveTool::Move && tool != ScreenshotActiveTool::Eraser &&
           tool != ScreenshotActiveTool::RectangleEraser &&
           tool != ScreenshotActiveTool::BrushEraser && tool != ScreenshotActiveTool::Spotlight &&
           tool != ScreenshotActiveTool::Watermark && !recognitionTool(tool);
}
} // namespace

bool ScreenshotInteractionState::beginEffectDrag(EffectGesture gesture) {
    if (!movingSelection() || !moveToolActive() || dragging() || m_effectEditorsSuppressed ||
        gesture.handle == ScreenshotSelectionEffectHandle::None)
        return false;
    m_hoveredEffectHandle = gesture.handle;
    m_effectGesture = std::move(gesture);
    return true;
}

void ScreenshotInteractionState::finishEffectDrag() {
    m_effectGesture.reset();
}

bool ScreenshotInteractionState::cancelEffectDrag() {
    if (!m_effectGesture)
        return false;
    auto gesture = std::move(*m_effectGesture);
    m_effectGesture.reset();
    m_hoveredEffectHandle = ScreenshotSelectionEffectHandle::None;
    if (gesture.rollback)
        gesture.rollback();
    return true;
}

void ScreenshotInteractionState::resetEffectEditors() {
    static_cast<void>(cancelEffectDrag());
    m_hoveredEffectHandle = ScreenshotSelectionEffectHandle::None;
}

void ScreenshotInteractionState::reset() {
    resetEffectEditors();
    m_effectEditorsSuppressed = false;
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = ScreenshotCaptureMode::Inactive;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::beginCapture() {
    resetEffectEditors();
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::enterOverlayVisible(bool selectorReady) {
    resetEffectEditors();
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = selectorReady ? ScreenshotCaptureMode::IntelligentSelecting
                           : ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::setMoveTool(bool hasSelection, bool selectorReady) {
    resetEffectEditors();
    m_activeTool = ScreenshotActiveTool::Move;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
    if (hasSelection) {
        m_mode = ScreenshotCaptureMode::MovingSelection;
        return;
    }
    m_mode = selectorReady ? ScreenshotCaptureMode::IntelligentSelecting
                           : ScreenshotCaptureMode::ManualSelecting;
}

void ScreenshotInteractionState::setCanvasTool(ScreenshotActiveTool tool) {
    resetEffectEditors();
    m_activeTool = tool;
    m_mode = ScreenshotCaptureMode::Editing;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = recognitionTool(tool);
}

void ScreenshotInteractionState::setOcrTool() {
    setCanvasTool(ScreenshotActiveTool::Ocr);
}

void ScreenshotInteractionState::setTableTool() {
    setCanvasTool(ScreenshotActiveTool::Table);
}

void ScreenshotInteractionState::setQrTool() {
    setCanvasTool(ScreenshotActiveTool::Qr);
}

void ScreenshotInteractionState::confirmSelection() {
    if (dragging()) {
        return;
    }
    m_mode = ScreenshotCaptureMode::MovingSelection;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
}

void ScreenshotInteractionState::applySelectionParams() {
    confirmSelection();
}

void ScreenshotInteractionState::enterScrollingCapture() {
    resetEffectEditors();
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = ScreenshotCaptureMode::ScrollingCapture;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::returnToSelectionMode(bool selectorReady) {
    resetEffectEditors();
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = selectorReady ? ScreenshotCaptureMode::IntelligentSelecting
                           : ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

bool ScreenshotInteractionState::enterSelectionDrag(ScreenshotSelectionDragMode dragMode) {
    resetEffectEditors();
    if (dragMode == ScreenshotSelectionDragMode::None) {
        return false;
    }

    if (!m_dragging)
        m_marqueeGesture = dragMode == ScreenshotSelectionDragMode::Marquee;
    // A selection is unconfirmed for the entire create/move/resize transaction.
    m_mode = ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = dragMode;
    m_dragging = true;
    return true;
}

void ScreenshotInteractionState::finishDrag() {
    resetEffectEditors();
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
}

void ScreenshotInteractionState::cancelDrag() {
    finishDrag();
}

ScreenshotActiveTool ScreenshotInteractionState::activeTool() const {
    return m_activeTool;
}

ScreenshotCaptureMode ScreenshotInteractionState::mode() const {
    return m_mode;
}

ScreenshotSelectionDragMode ScreenshotInteractionState::dragMode() const {
    return m_dragMode;
}

bool ScreenshotInteractionState::dragging() const {
    return m_dragging || m_effectGesture.has_value();
}

bool ScreenshotInteractionState::inactive() const {
    return m_mode == ScreenshotCaptureMode::Inactive;
}

bool ScreenshotInteractionState::moveToolActive() const {
    return m_activeTool == ScreenshotActiveTool::Move;
}

bool ScreenshotInteractionState::intelligentSelecting() const {
    return m_mode == ScreenshotCaptureMode::IntelligentSelecting;
}

bool ScreenshotInteractionState::manualSelecting() const {
    return m_mode == ScreenshotCaptureMode::ManualSelecting;
}

bool ScreenshotInteractionState::marqueeSelecting() const {
    return manualSelecting() && (!m_dragging || m_dragMode == ScreenshotSelectionDragMode::Marquee);
}

bool ScreenshotInteractionState::modifyingSelection() const {
    return manualSelecting() && m_dragging && m_dragMode != ScreenshotSelectionDragMode::Marquee;
}

bool ScreenshotInteractionState::movingSelection() const {
    return m_mode == ScreenshotCaptureMode::MovingSelection;
}

bool ScreenshotInteractionState::editing() const {
    return m_mode == ScreenshotCaptureMode::Editing;
}

bool ScreenshotInteractionState::scrollingCapture() const {
    return m_mode == ScreenshotCaptureMode::ScrollingCapture;
}

bool ScreenshotInteractionState::selecting() const {
    return intelligentSelecting() || manualSelecting();
}

bool ScreenshotInteractionState::preselectionActive(
    const ScreenshotSelectionModel& selection) const {
    if (!selecting() || m_dragging || selection.constructionActive() ||
        selection.regionOperationActive()) {
        return false;
    }
    return intelligentSelecting() || !selection.hasPixelSelection();
}

bool ScreenshotInteractionState::cursorMovementEnabled() const {
    if (!selecting() && !movingSelection() && !editing()) {
        return false;
    }
    return moveToolActive() || (editing() && drawingToolSupportsCursorMovement(m_activeTool));
}

bool ScreenshotInteractionState::selectionToolbarMode() const {
    return intelligentSelecting() || manualSelecting() || movingSelection() || editing();
}

bool ScreenshotInteractionState::canResizeSelection() const {
    return movingSelection() || editing();
}

bool ScreenshotInteractionState::selectionHandlesVisible() const {
    return !m_recognitionSelectionActive &&
           (!manualSelecting() || (m_dragging && !m_marqueeGesture));
}
