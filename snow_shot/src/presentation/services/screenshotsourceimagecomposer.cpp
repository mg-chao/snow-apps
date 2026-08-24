#include "snow_shot/presentation/screenshotsourceimagecomposer.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_draw_engine_qt/snow_transient_image.h"

#include <QPainter>

QImage composeScreenshotSourceSelection(const ScreenshotDisplaySession& displaySession,
                                        const QRect& selection) {
    if (selection.width() < 1 || selection.height() < 1) {
        return {};
    }

    QImage image = snow_draw_engine_qt::allocateTransientImage(selection.size(),
                                                               QImage::Format_RGBA8888);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF selectionRect(selection);
    displaySession.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        const QRectF canvasRect = ScreenshotGeometryMapper::displayImageSourceCanvasRect(display);
        if (display.image.isNull() || !canvasRect.intersects(selectionRect)) {
            return;
        }
        const QRectF targetRect = canvasRect.translated(-static_cast<qreal>(selection.left()),
                                                        -static_cast<qreal>(selection.top()));
        painter.drawImage(targetRect, display.image);
    });
    return image;
}
