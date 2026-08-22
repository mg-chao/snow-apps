#include "snow_shot/presentation/screenshotinteractionstate.h"

namespace {
bool recognitionTool(ScreenshotActiveTool tool) {
    return tool == ScreenshotActiveTool::Ocr || tool == ScreenshotActiveTool::Table ||
           tool == ScreenshotActiveTool::Qr;
}

bool drawingToolSupportsCursorMovement(ScreenshotActiveTool tool) {
    return tool != ScreenshotActiveTool::Move && tool != ScreenshotActiveTool::Eraser &&
           tool != ScreenshotActiveTool::Spotlight && tool != ScreenshotActiveTool::Watermark &&
           !recognitionTool(tool);
}
} // namespace

void ScreenshotInteractionState::reset() {
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = ScreenshotCaptureMode::Inactive;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::beginCapture() {
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::enterOverlayVisible(bool selectorReady) {
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = selectorReady ? ScreenshotCaptureMode::IntelligentSelecting
                           : ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::setMoveTool(bool hasSelection, bool selectorReady) {
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
    if (m_dragging) {
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
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = ScreenshotCaptureMode::ScrollingCapture;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

void ScreenshotInteractionState::returnToSelectionMode(bool selectorReady) {
    m_activeTool = ScreenshotActiveTool::Move;
    m_mode = selectorReady ? ScreenshotCaptureMode::IntelligentSelecting
                           : ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = ScreenshotSelectionDragMode::None;
    m_dragging = false;
    m_recognitionSelectionActive = false;
}

bool ScreenshotInteractionState::enterSelectionDrag(ScreenshotSelectionDragMode dragMode) {
    if (dragMode == ScreenshotSelectionDragMode::None) {
        return false;
    }

    // A selection is unconfirmed for the entire create/move/resize transaction.
    m_mode = ScreenshotCaptureMode::ManualSelecting;
    m_dragMode = dragMode;
    m_dragging = true;
    return true;
}

void ScreenshotInteractionState::finishDrag() {
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
    return m_dragging;
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
    return manualSelecting() &&
           (!m_dragging || m_dragMode == ScreenshotSelectionDragMode::Marquee);
}

bool ScreenshotInteractionState::modifyingSelection() const {
    return manualSelecting() && m_dragging &&
           m_dragMode != ScreenshotSelectionDragMode::Marquee;
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
    return !m_recognitionSelectionActive;
}
