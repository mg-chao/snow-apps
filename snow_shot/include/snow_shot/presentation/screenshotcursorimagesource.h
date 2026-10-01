#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCURSORIMAGESOURCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCURSORIMAGESOURCE_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshottypes.h"

[[nodiscard]] inline QRectF screenshotCursorCanvasRect(const CapturedDisplayModel& display) {
    if (display.image.isNull() || display.cursorPatch.isNull() ||
        display.cursorPatch.size() != display.cursorPixelRect.size() ||
        !display.image.rect().contains(display.cursorPixelRect))
        return {};
    const QRectF source = ScreenshotGeometryMapper::displayImageSourceCanvasRect(display);
    const qreal sx = source.width() / display.image.width();
    const qreal sy = source.height() / display.image.height();
    const QRect& rect = display.cursorPixelRect;
    return {source.x() + rect.x() * sx, source.y() + rect.y() * sy, rect.width() * sx,
            rect.height() * sy};
}

[[nodiscard]] inline QList<ScreenshotImageLayer>
screenshotDisplayImageLayers(const CapturedDisplayModel& display, bool cursorVisible) {
    const QRectF rect = ScreenshotGeometryMapper::displayImageSourceCanvasRect(display);
    QList<ScreenshotImageLayer> layers;
    if (!display.image.isNull())
        layers.push_back({display.image, rect, rect});
    if (cursorVisible) {
        const QRectF cursor = screenshotCursorCanvasRect(display);
        if (!cursor.isEmpty())
            layers.push_back({display.cursorPatch, cursor, cursor, false});
    }
    return layers;
}

[[nodiscard]] inline ScreenshotImageSource
screenshotDisplayImageSource(const CapturedDisplayModel& display, bool cursorVisible) {
    if (cursorVisible && !screenshotCursorCanvasRect(display).isEmpty())
        return ScreenshotImageSource::fromLayers(screenshotDisplayImageLayers(display, true));
    return ScreenshotImageSource::fromImage(
        display.image, ScreenshotGeometryMapper::displayImageSourceCanvasRect(display));
}

#endif
