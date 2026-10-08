#pragma once

#include <QFont>
#include <QPointF>
#include <QString>

#include "snow_canvas_display_item.h"
#include "snow_draw_engine.h"

#include <optional>

namespace snow_canvas_text {

double defaultTextFontSize();
double minimumTextFontSize();
double resolvedTextFontSize(double fontSize);
QString textFromSceneItem(const SnowSceneDisplayItem& item);
QString textFromElementInfo(const SnowTextElementInfo& info);
std::optional<QString> completeTextFromElementInfo(const SnowTextElementInfo& info,
                                                   SnowRuntime runtime);
QString fontFamilyFromSceneItem(const SnowSceneDisplayItem& item);
void copyTextToSceneItem(SnowCanvasSceneItem& item, const QString& text);
void applyTextStyleToSceneItem(SnowCanvasSceneItem& item, const SnowTextStyle& style);
SnowTextStyle textStyleFromSceneItem(const SnowSceneDisplayItem& item);
SnowTextStyle patchedTextStyle(SnowTextStyle current, const SnowTextStyle& requested,
                               std::uint32_t properties);

SnowTextElementInfo newTextInfoAt(const QPointF& canvasPoint, const QFont& baseFont,
                                  const SnowTextStyle& style);
SnowCanvasSceneItem defaultPreviewItem(const SnowTextElementInfo& info);
void updatePreviewFromEditorText(SnowCanvasSceneItem& item, const QString& text, bool autoResize,
                                 const QFont& baseFont,
                                 std::optional<double> maximumWidth = std::nullopt);

} // namespace snow_canvas_text
