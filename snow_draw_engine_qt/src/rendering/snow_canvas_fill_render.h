#pragma once

#include "snow_draw_engine.h"

#include <QColor>

#include <cstddef>

class QPainter;
class QPainterPath;

namespace snow_canvas_fill_render {

void drawStyledFill(QPainter& painter, const QPainterPath& path, const SnowColorRgba8& fill,
                    SnowFillStyle style, double referenceStrokeWidth, double coordinateScale = 1.0);

void drawTextBackgroundFill(QPainter& painter, const QPainterPath& path, const SnowColorRgba8& fill,
                            SnowFillStyle style, double fontSize, double coordinateScale = 1.0);

std::size_t hatchTextureCacheEntryCountForCurrentThread();
void resetHatchTextureCacheForCurrentThread();

} // namespace snow_canvas_fill_render
