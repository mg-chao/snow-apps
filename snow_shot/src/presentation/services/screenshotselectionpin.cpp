#include "snow_shot/presentation/screenshotselectionpin.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

ScreenshotPinnedSelectionRequest
screenshotSelectionPinRequest(const ScreenshotDisplaySession& displaySession,
                              const ScreenshotGeometryMapper& geometry, const QRect& selection,
                              const ScreenshotResultStyle& style) {
    ScreenshotPinnedSelectionRequest request;
    request.resultStyle = ScreenshotResultCompositor::normalizedStyle(style);
    const ScreenshotResultLayout layout =
        ScreenshotResultCompositor::layoutForContent(selection.size(), request.resultStyle);
    if (!layout.isValid()) {
        return request;
    }
    const int shadowPadding = layout.effectInsets.left();
    const ScreenshotPinnedImagePlacement placement = geometry.pinnedImagePlacement(
        displaySession, selection, layout.outputRect.size(), shadowPadding);
    if (!placement.valid) {
        return request;
    }
    request.selection = selection;
    request.contentCanvasRect = QRectF(selection);
    request.surfaceCanvasRect = request.contentCanvasRect.adjusted(
        -static_cast<qreal>(shadowPadding), -static_cast<qreal>(shadowPadding),
        static_cast<qreal>(shadowPadding), static_cast<qreal>(shadowPadding));
    request.geometry = placement.geometry;
    request.geometry.canvasSourceRect = request.surfaceCanvasRect;
    request.initialWindowSize = layout.outputRect.size();
    request.screen = placement.screen;
    return request;
}
