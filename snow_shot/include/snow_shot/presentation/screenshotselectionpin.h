#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H

#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"
#include "snow_shot/storage/pinnedwindowtypes.h"

class ScreenshotDisplaySession;

// The image already contains its effects. Retain their outline independently.
[[nodiscard]] inline snow_shot::storage::PinnedBorderAppearance
screenshotSelectionBorderAppearance(const QSize& contentSize, const ScreenshotResultStyle& style) {
    const auto normalized = ScreenshotResultCompositor::normalizedStyle(style);
    const auto layout = ScreenshotResultCompositor::layoutForContent(contentSize, normalized);
    return {layout.outputRect.size(), QRectF(layout.contentRect),
            static_cast<qreal>(normalized.cornerRadius), normalized.shadowWidth > 0};
}

// Window geometry for one composited selection. Live Pin to Screen and history pins both
// use this request; the bitmap is mapped onto surfaceCanvasRect, whose size is the window.
[[nodiscard]] ScreenshotPinnedSelectionRequest
screenshotSelectionPinRequest(const ScreenshotDisplaySession& displays,
                              const ScreenshotGeometryMapper& geometry, const QRect& selection,
                              const ScreenshotResultStyle& style);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H
