#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARCOMMANDS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARCOMMANDS_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include <QString>

namespace adqt::widgets {
class AdColorPicker;
}

class ScreenshotToolbarCommandSink {
  public:
    virtual ~ScreenshotToolbarCommandSink() = default;

    virtual void undoCanvasEdit() {}
    virtual void redoCanvasEdit() {}
    virtual void setMoveTool() = 0;
    virtual void setSelectTool() = 0;
    virtual void setShapeTool() = 0;
    virtual void setArrowTool() = 0;
    virtual void setLineTool() = 0;
    virtual void setFreeDrawTool() = 0;
    virtual void setHighlightTool() = 0;
    virtual void setPenHighlightTool() = 0;
    virtual void setSpotlightTool() {}
    virtual void setEraserTool() = 0;
    virtual void setFilterTool() = 0;
    virtual void setRectangleFilterTool() {
        setFilterTool();
    }
    virtual void setPenFilterTool() {}
    virtual void setWatermarkTool() = 0;
    virtual void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig& config) = 0;
    virtual void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig& config) = 0;
    virtual void setSpotlightConfigFromToolbar(const SnowCanvasSpotlightConfig&) {}
    virtual void previewSpotlightFromToolbar(const SnowCanvasSpotlightConfig&) {}
    virtual void setFilterStyleFromToolbar(const SnowCanvasFilterStyle& style,
                                           quint32 properties) = 0;
    virtual void setTextTool() = 0;
    virtual void setSerialNumberTool() = 0;
    virtual void setOcrTool() = 0;
    virtual void setTextTranslationTool() { setOcrTool(); }
    virtual void setTableTool() {}
    virtual void setQrTool() {}
    virtual void mergeTableSelection() {}
    virtual void splitTableSelection() {}
    virtual void resetTable() {}
    virtual void beginTextEditing() {}
    virtual void toggleTextEditing() { beginTextEditing(); }
    virtual void beginTextTranslation() {}
    virtual void toggleTextTranslation() { beginTextTranslation(); }
    virtual void resetTextEditing() {}
    virtual void openTextTranslationSettings() {}
    virtual void applyTextFormatting(const QString&) {}
    virtual void applyTextPunctuation(const QString&) {}
    virtual void startScrollingScreenshot() = 0;
    virtual void setScrollingScreenshotRecognitionMode(ScreenshotScrollingRecognitionMode) {}
    virtual void pinSelectionToScreen() = 0;
    virtual void saveSelectionToFile() {}
    virtual void cancelCapture() = 0;
    virtual void copySelectionToClipboard() = 0;
    virtual void startScreenRecording() = 0;
    virtual void setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style, quint32 properties,
                                          SnowCanvasShapeKind kind) = 0;
    virtual void setTextStyleFromToolbar(const SnowCanvasTextStyle& style) = 0;
    virtual void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle& style) = 0;
    virtual void decrementSelectedSerialNumbers() = 0;
    virtual void incrementSelectedSerialNumbers() = 0;
    virtual void createTextForSelectedSerialNumber() = 0;
    virtual void reorderSelectedElements(SnowCanvasSelectionOrder) {}
    virtual void setSelectedElementsOpacity(qreal) {}
    virtual void duplicateSelectedElements() {}
    virtual void deleteSelectedElements() {}
    virtual void repositionToolbarForContentChange() = 0;
    virtual void hideColorPickersForScreenshotUi() = 0;
    virtual void beginCanvasColorSampling(adqt::widgets::AdColorPicker*) {}
};

class ScreenshotSelectionToolbarCommandSink {
  public:
    virtual ~ScreenshotSelectionToolbarCommandSink() = default;

    virtual void toggleSelectionAspectRatioLockFromToolbar() = 0;
    virtual void openSelectionResizeModalFromToolbar() = 0;
    virtual void hideColorPickersForScreenshotUi() = 0;
    virtual void adjustSelectionFromToolbar(int minDx, int minDy, int maxDx, int maxDy) = 0;
    virtual void setSelectionCornerRadiusFromToolbar(int radius) = 0;
    virtual void setSelectionShadowWidthFromToolbar(int shadowWidth) = 0;
    virtual void setSelectionToolbarHovered(bool hovered) = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARCOMMANDS_H
