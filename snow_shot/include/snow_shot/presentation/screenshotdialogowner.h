#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTDIALOGOWNER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTDIALOGOWNER_H

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"

[[nodiscard]] inline ScreenshotOverlayWindow*
screenshotSelectionDialogOwner(const ScreenshotDisplaySession& displays,
                               const ScreenshotGeometryMapper& geometry, const QRectF& selection,
                               ScreenshotOverlayWindow* keyboardOwner) {
    if (!selection.isEmpty()) {
        const auto* display = geometry.displayForCanvasRect(displays, selection);
        if (auto* overlay = displays.overlayForDisplay(display)) {
            return overlay;
        }
    }
    return keyboardOwner;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTDIALOGOWNER_H
