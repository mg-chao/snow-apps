#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASTOOLSTYLES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASTOOLSTYLES_H

#include "snow_draw_engine_qt/snow_canvas_style_edit.h"

class SnowCanvasWidget;

namespace snow_shot::presentation {

[[nodiscard]] SnowCanvasStyleDefaults screenshotCanvasToolStyleDefaults();
[[nodiscard]] bool persistScreenshotCanvasToolStyles(const SnowCanvasStyleDefaults& defaults);
[[nodiscard]] bool persistScreenshotCanvasStyleEdit(const SnowCanvasStyleEdit& edit);
void applyScreenshotCanvasToolStyles(SnowCanvasWidget& canvas,
                                     const SnowCanvasStyleDefaults& defaults);

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASTOOLSTYLES_H
