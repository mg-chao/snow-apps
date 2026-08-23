#include "snow_shot/presentation/screenshottoolcommandworkflow.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include <QCursor>

#include <utility>

ScreenshotToolCommandWorkflow::ScreenshotToolCommandWorkflow(
    ScreenshotToolCommandWorkflowContext context)
    : m_context(std::move(context)) {}

void ScreenshotToolCommandWorkflow::setMoveTool() {
    const QRect selection = m_context.selection.pixelSelection();
    const bool hasSelection = selection.width() >= 1 && selection.height() >= 1;
    const bool selectorReady = m_context.actions.selectorReady();
    m_context.interaction.setMoveTool(hasSelection, selectorReady);
    m_context.actions.setCanvasInteractionEnabled(false);
    if (hasSelection) {
        m_context.actions.updateOverlayState();
        return;
    }

    if (selectorReady) {
        m_context.actions.updateOverlayCursors();
        m_context.actions.updateSelectorSelectionAt(
            m_context.geometry.physicalPositionForLogicalPoint(m_context.displaySession,
                                                               QCursor::pos()));
        return;
    }

    m_context.actions.clearSelectorSelection();
    m_context.actions.updateOverlayState();
    m_context.actions.startSelectorRefresh();
}

void ScreenshotToolCommandWorkflow::setSelectTool() {
    setCanvasTool(ScreenshotActiveTool::Select, SnowCanvasTool::Select);
}

void ScreenshotToolCommandWorkflow::setShapeTool() {
    setCanvasTool(ScreenshotActiveTool::Shape, SnowCanvasTool::Shape);
}

void ScreenshotToolCommandWorkflow::setArrowTool() {
    setCanvasTool(ScreenshotActiveTool::Arrow, SnowCanvasTool::Arrow);
}

void ScreenshotToolCommandWorkflow::setLineTool() {
    setCanvasTool(ScreenshotActiveTool::Line, SnowCanvasTool::Line);
}

void ScreenshotToolCommandWorkflow::setFreeDrawTool() {
    setCanvasTool(ScreenshotActiveTool::FreeDraw, SnowCanvasTool::FreeDraw);
}

void ScreenshotToolCommandWorkflow::setHighlightTool() {
    setCanvasTool(ScreenshotActiveTool::RectangleHighlight, SnowCanvasTool::RectangleHighlight);
}

void ScreenshotToolCommandWorkflow::setPenHighlightTool() {
    setCanvasTool(ScreenshotActiveTool::PenHighlight, SnowCanvasTool::PenHighlight);
}

void ScreenshotToolCommandWorkflow::setEraserTool() {
    setCanvasTool(ScreenshotActiveTool::Eraser, SnowCanvasTool::Eraser);
}

void ScreenshotToolCommandWorkflow::setFilterTool() {
    setRectangleFilterTool();
}

void ScreenshotToolCommandWorkflow::setSpotlightTool() {
    setCanvasTool(ScreenshotActiveTool::Spotlight, SnowCanvasTool::Spotlight);
}

void ScreenshotToolCommandWorkflow::setRectangleFilterTool() {
    setCanvasTool(ScreenshotActiveTool::RectangleFilter, SnowCanvasTool::RectangleFilter);
}

void ScreenshotToolCommandWorkflow::setPenFilterTool() {
    setCanvasTool(ScreenshotActiveTool::PenFilter, SnowCanvasTool::PenFilter);
}

void ScreenshotToolCommandWorkflow::setWatermarkTool() {
    setCanvasTool(ScreenshotActiveTool::Watermark, SnowCanvasTool::Watermark);
}

void ScreenshotToolCommandWorkflow::setWatermarkConfigFromToolbar(
    const SnowCanvasWatermarkConfig& config) {
    m_context.actions.setWatermarkConfig(config);
}

void ScreenshotToolCommandWorkflow::setSpotlightConfigFromToolbar(
    const SnowCanvasSpotlightConfig& config) {
    m_context.actions.setSpotlightConfig(config);
}

void ScreenshotToolCommandWorkflow::setFilterStyleFromToolbar(const SnowCanvasFilterStyle& style,
                                                              quint32 properties) {
    m_context.actions.setFilterStyle(style, properties);
}

void ScreenshotToolCommandWorkflow::setTextTool() {
    setCanvasTool(ScreenshotActiveTool::Text, SnowCanvasTool::Text);
}

void ScreenshotToolCommandWorkflow::setSerialNumberTool() {
    setCanvasTool(ScreenshotActiveTool::SerialNumber, SnowCanvasTool::SerialNumber);
}

void ScreenshotToolCommandWorkflow::decrementSelectedSerialNumbers() {
    m_context.actions.adjustSelectedSerialNumbers(-1);
}

void ScreenshotToolCommandWorkflow::incrementSelectedSerialNumbers() {
    m_context.actions.adjustSelectedSerialNumbers(1);
}

void ScreenshotToolCommandWorkflow::createTextForSelectedSerialNumber() {
    m_context.actions.createCanvasTextForSelectedSerialNumber();
}

SnowCanvasShapeStyle ScreenshotToolCommandWorkflow::currentRectangleStyle() const {
    SnowCanvasShapeStyle style;
    return m_context.actions.tryCurrentRectangleStyle(&style) ? style : SnowCanvasShapeStyle{};
}

void ScreenshotToolCommandWorkflow::setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style,
                                                             quint32 properties,
                                                             SnowCanvasShapeKind kind) {
    m_context.actions.setShapeStylePatch(style, properties, kind);
}

void ScreenshotToolCommandWorkflow::setTextStyleFromToolbar(const SnowCanvasTextStyle& style) {
    m_context.actions.setTextStyle(style);
}

void ScreenshotToolCommandWorkflow::setSerialNumberStyleFromToolbar(
    const SnowCanvasSerialNumberStyle& style) {
    m_context.actions.setSerialNumberStyle(style);
}

bool ScreenshotToolCommandWorkflow::stepStrokeWidth(int delta) {
    return m_context.actions.stepToolbarStrokeWidth(delta);
}

void ScreenshotToolCommandWorkflow::setCanvasTool(ScreenshotActiveTool activeTool,
                                                  SnowCanvasTool canvasTool) {
    m_context.interaction.setCanvasTool(activeTool);
    m_context.captureState.sessionState = ScreenshotSessionState::Editing;
    m_context.intelligentSelection.clearPress();
    m_context.actions.setCanvasInteractionEnabled(true);
    m_context.actions.setCanvasTool(canvasTool);
    m_context.actions.updateOverlayState();
    m_context.actions.updateOverlayCursors();
    m_context.actions.raiseToolbarForCanvasInteraction();
}
