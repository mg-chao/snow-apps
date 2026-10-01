#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H

#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/presentation/screenshotclipboardappearance.h"

class ScreenshotDisplaySession;

// The image already contains its effects. Retain their outline independently.
[[nodiscard]] inline snow_shot::storage::PinnedBorderAppearance
screenshotSelectionBorderAppearance(const QSize& contentSize, const ScreenshotResultStyle& style) {
    const auto normalized = ScreenshotResultCompositor::normalizedStyle(style);
    const auto layout = ScreenshotResultCompositor::layoutForContent(contentSize, normalized);
    return {layout.outputRect.size(), QRectF(layout.contentRect),
            static_cast<qreal>(normalized.cornerRadius), normalized.shadowWidth > 0,
            normalized.region};
}

[[nodiscard]] inline bool screenshotSelectionNeedsCheckerboard(
    const std::optional<snow_shot::storage::PinnedBorderAppearance>& appearance) {
    return appearance && appearance->region &&
           (appearance->region->custom() || appearance->region->rectCount() != 1 ||
            appearance->region->boundingRect() !=
                QRect(QPoint(), appearance->contentRect.size().toSize()));
}

// Snapshot the presentation of an already composited selection. Raster size is
// finalized by the export worker, independently of the platform window units.
[[nodiscard]] inline ScreenshotClipboardAppearance
screenshotSelectionClipboardAppearance(const QSize& contentSize,
                                       const ScreenshotResultStyle& style) {
    ScreenshotClipboardAppearance result;
    result.borderAppearance = screenshotSelectionBorderAppearance(contentSize, style);
    result.rasterSize = result.borderAppearance->sourceSize;
    result.checkerboardEnabled = screenshotSelectionNeedsCheckerboard(result.borderAppearance);
    return result;
}

// Window geometry for one composited selection. Live Pin to Screen and history pins both
// use this request; the bitmap is mapped onto surfaceCanvasRect, whose size is the window.
[[nodiscard]] ScreenshotPinnedSelectionRequest
screenshotSelectionPinRequest(const ScreenshotDisplaySession& displays,
                              const ScreenshotGeometryMapper& geometry, const QRect& selection,
                              const ScreenshotResultStyle& style);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPIN_H
