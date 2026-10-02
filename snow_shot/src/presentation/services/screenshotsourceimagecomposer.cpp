#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotcursorimagesource.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#include <QPainter>
#include <QColorSpace>

QImage composeScreenshotSourceSelection(const ScreenshotDisplaySession& displaySession,
                                        const QRect& selection) {
    if (selection.width() < 1 || selection.height() < 1) {
        return {};
    }

    const auto spec = screenshotSelectionRenderSpec(displaySession, selection);
    if (!spec.isValid())
        return {};
    QImage image = snowCanvasAllocateImage(spec.pixelSize, QImage::Format_RGBA8888);
    if (image.isNull())
        return {};
    image.setColorSpace(QColorSpace::SRgb);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.scale(spec.scale, spec.scale);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF selectionRect(selection);
    displaySession.forEachImageSource([&](qsizetype, const CapturedDisplayModel& display) {
        const QRectF canvasRect = ScreenshotGeometryMapper::displayImageSourceCanvasRect(display);
        if (display.image.isNull() || !canvasRect.intersects(selectionRect)) {
            return;
        }
        for (const auto& layer :
             screenshotDisplayImageLayers(display, displaySession.cursorVisible)) {
            const QRectF targetRect = layer.destinationCanvasRect.translated(
                -static_cast<qreal>(selection.left()), -static_cast<qreal>(selection.top()));
            painter.drawImage(targetRect, layer.image);
        }
    });
    return image;
}
