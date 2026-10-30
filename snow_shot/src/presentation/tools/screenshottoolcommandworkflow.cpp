#include "snow_shot/presentation/screenshotcanvastoolmapping.h"
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
    setCanvasTool(SnowCanvasTool::Select);
}

void ScreenshotToolCommandWorkflow::setSpeechBubbleTool() {
    setCanvasTool(SnowCanvasTool::SpeechBubble);
}

void ScreenshotToolCommandWorkflow::setShapeTool() {
    setCanvasTool(SnowCanvasTool::Shape);
}

void ScreenshotToolCommandWorkflow::setArrowTool() {
    setCanvasTool(SnowCanvasTool::Arrow);
}

void ScreenshotToolCommandWorkflow::setLineTool() {
    setCanvasTool(SnowCanvasTool::Line);
}

void ScreenshotToolCommandWorkflow::setDistanceTool() {
    setCanvasTool(SnowCanvasTool::Distance);
}

void ScreenshotToolCommandWorkflow::setAngleTool() {
    setCanvasTool(SnowCanvasTool::Angle);
}

void ScreenshotToolCommandWorkflow::setBracketTool() {
    setCanvasTool(SnowCanvasTool::Bracket);
}

void ScreenshotToolCommandWorkflow::setFreeDrawTool() {
    setCanvasTool(SnowCanvasTool::FreeDraw);
}

void ScreenshotToolCommandWorkflow::setHighlightTool() {
    setCanvasTool(SnowCanvasTool::RectangleHighlight);
}

void ScreenshotToolCommandWorkflow::setPenHighlightTool() {
    setCanvasTool(SnowCanvasTool::PenHighlight);
}

void ScreenshotToolCommandWorkflow::setEraserTool() {
    setCanvasTool(SnowCanvasTool::Eraser);
}

void ScreenshotToolCommandWorkflow::setRectangleEraserTool() {
    setCanvasTool(SnowCanvasTool::RectangleEraser);
}

void ScreenshotToolCommandWorkflow::setBrushEraserTool() {
    setCanvasTool(SnowCanvasTool::BrushEraser);
}

void ScreenshotToolCommandWorkflow::setFilterTool() {
    setRectangleFilterTool();
}

void ScreenshotToolCommandWorkflow::setMagnifierTool() {
    setCanvasTool(SnowCanvasTool::Magnifier);
}

void ScreenshotToolCommandWorkflow::setSpotlightTool() {
    setCanvasTool(SnowCanvasTool::Spotlight);
}

void ScreenshotToolCommandWorkflow::setRectangleFilterTool() {
    setCanvasTool(SnowCanvasTool::RectangleFilter);
}

void ScreenshotToolCommandWorkflow::setAutoFilterTool() {
    setCanvasTool(SnowCanvasTool::AutoFilter);
}

void ScreenshotToolCommandWorkflow::setPenFilterTool() {
    setCanvasTool(SnowCanvasTool::PenFilter);
}

void ScreenshotToolCommandWorkflow::setWatermarkTool() {
    setCanvasTool(SnowCanvasTool::Watermark);
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
    setCanvasTool(SnowCanvasTool::Text);
}

void ScreenshotToolCommandWorkflow::setSerialNumberTool() {
    setCanvasTool(SnowCanvasTool::SerialNumber);
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

void ScreenshotToolCommandWorkflow::setTextStyleFromToolbar(const SnowCanvasTextStyle& style,
                                                            quint32 properties) {
    m_context.actions.setTextStyle(style, properties);
}

void ScreenshotToolCommandWorkflow::setSerialNumberStyleFromToolbar(
    const SnowCanvasSerialNumberStyle& style, std::optional<quint32> properties) {
    m_context.actions.setSerialNumberStyle(style, properties);
}

bool ScreenshotToolCommandWorkflow::stepStrokeWidth(int delta) {
    return m_context.actions.stepToolbarStrokeWidth(delta);
}

void ScreenshotToolCommandWorkflow::setCanvasTool(SnowCanvasTool canvasTool) {
    const auto activeTool = snow_shot::presentation::activeToolForCanvasTool(canvasTool);
    if (!activeTool)
        return;
    m_context.interaction.setCanvasTool(*activeTool);
    m_context.captureState.sessionState = ScreenshotSessionState::Editing;
    m_context.intelligentSelection.clearPress();
    m_context.actions.setCanvasInteractionEnabled(true);
    m_context.actions.setCanvasTool(canvasTool);
    m_context.actions.updateOverlayState();
    m_context.actions.updateOverlayCursors();
    m_context.actions.raiseToolbarForCanvasInteraction();
}
