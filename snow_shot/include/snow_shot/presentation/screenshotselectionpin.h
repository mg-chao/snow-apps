#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H

#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"

class ScreenshotDisplaySession;

// Window geometry for one composited selection. Live Pin to Screen and history pins both
// use this request; the bitmap is mapped onto surfaceCanvasRect, whose size is the window.
[[nodiscard]] ScreenshotPinnedSelectionRequest
screenshotSelectionPinRequest(const ScreenshotDisplaySession& displays,
                              const ScreenshotGeometryMapper& geometry, const QRect& selection,
                              const ScreenshotResultStyle& style);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H
