#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLCOMMANDWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLCOMMANDWORKFLOW_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshottoolcommandworkflowports.h"

class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotIntelligentSelectionModel;
class ScreenshotSelectionModel;
struct ScreenshotCaptureState;

struct ScreenshotToolCommandWorkflowContext {
    ScreenshotCaptureState& captureState;
    ScreenshotToolCommandActions actions;
    ScreenshotDisplaySession& displaySession;
    const ScreenshotGeometryMapper& geometry;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotIntelligentSelectionModel& intelligentSelection;
};

class ScreenshotToolCommandWorkflow final {
  public:
    explicit ScreenshotToolCommandWorkflow(ScreenshotToolCommandWorkflowContext context);

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
    void setAutoFilterTool();
    void setWatermarkTool();
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig& config);
    void setSpotlightConfigFromToolbar(const SnowCanvasSpotlightConfig& config);
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle& style, quint32 properties);
    void setTextTool();
    void setSerialNumberTool();

    void decrementSelectedSerialNumbers();
    void incrementSelectedSerialNumbers();
    void createTextForSelectedSerialNumber();

    [[nodiscard]] SnowCanvasShapeStyle currentRectangleStyle() const;
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style, quint32 properties,
                                  SnowCanvasShapeKind kind);
    void setTextStyleFromToolbar(const SnowCanvasTextStyle& style);
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle& style);
    [[nodiscard]] bool stepStrokeWidth(int delta);

  private:
    void setCanvasTool(ScreenshotActiveTool activeTool, SnowCanvasTool canvasTool);

    ScreenshotToolCommandWorkflowContext m_context;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLCOMMANDWORKFLOW_H
