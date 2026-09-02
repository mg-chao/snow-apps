#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLCOMMANDWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLCOMMANDWORKFLOWPORTS_H

#include <QPoint>
#include <QtGlobal>

#include <functional>

enum class SnowCanvasTool;
struct SnowCanvasShapeStyle;
struct SnowCanvasTextStyle;
struct SnowCanvasSerialNumberStyle;
struct SnowCanvasFilterStyle;
struct SnowCanvasWatermarkConfig;
struct SnowCanvasSpotlightConfig;
struct SnowCanvasStyleToolbarState;
enum class SnowCanvasShapeKind;

struct ScreenshotToolCommandActions {
    std::function<bool()> selectorReady;
    std::function<void()> startSelectorRefresh;
    std::function<void(const QPoint& physicalPoint)> updateSelectorSelectionAt;
    std::function<void()> clearSelectorSelection;

    std::function<void(bool enabled)> setCanvasInteractionEnabled;
    std::function<void(SnowCanvasTool tool)> setCanvasTool;
    std::function<bool(SnowCanvasShapeStyle* outStyle)> tryCurrentRectangleStyle;
    std::function<void(const SnowCanvasShapeStyle& style, quint32 properties,
                       SnowCanvasShapeKind kind)>
        setShapeStylePatch;
    std::function<void(const SnowCanvasFilterStyle& style, quint32 properties)> setFilterStyle;
    std::function<void(const SnowCanvasWatermarkConfig& config)> setWatermarkConfig;
    std::function<void(const SnowCanvasSpotlightConfig& config)> setSpotlightConfig;
    std::function<void(const SnowCanvasTextStyle& style)> setTextStyle;
    std::function<void(const SnowCanvasSerialNumberStyle& style)> setSerialNumberStyle;
    std::function<void(qint64 delta)> adjustSelectedSerialNumbers;
    std::function<void()> createCanvasTextForSelectedSerialNumber;
    std::function<bool(int direction)> stepToolbarStrokeWidth;

    std::function<void()> updateOverlayState;
    std::function<void()> updateOverlayCursors;
    std::function<void()> raiseToolbarForCanvasInteraction;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLCOMMANDWORKFLOWPORTS_H
