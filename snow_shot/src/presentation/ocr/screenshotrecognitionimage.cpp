#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QPainter>
#include <cmath>
#include <utility>

QImage renderScreenshotRecognitionImage(const ScreenshotRecognitionImageSnapshot& snapshot,
                                        const std::function<bool()>& cancelled) {
    const auto stopped = [&]() { return cancelled && cancelled(); };
    const QRectF rect = snapshot.canvasRect;
    if (snapshot.image.isNull() || !rect.isValid() || rect.isEmpty() || !std::isfinite(rect.x()) ||
        !std::isfinite(rect.y()) || !std::isfinite(rect.width()) || !std::isfinite(rect.height()) ||
        stopped()) {
        return {};
    }
    QImage image = snapshot.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1.0);
    QPainter painter(&image);
    if (!painter.isActive())
        return {};
    const QTransform canvasToPixels(
        image.width() / rect.width(), 0.0, 0.0, image.height() / rect.height(),
        -rect.x() * image.width() / rect.width(), -rect.y() * image.height() / rect.height());
    painter.setClipRect(image.rect());
    if (!snapshot.filteredImage.isNull() && snapshot.filteredCanvasRect.isValid()) {
        painter.drawImage(canvasToPixels.mapRect(snapshot.filteredCanvasRect),
                          snapshot.filteredImage);
    }
    ScreenshotOcrTextLayout layout;
    for (const ScreenshotOcrLine& line : snapshot.lines) {
        if (stopped())
            return {};
        if (!line.quad.boundingRect().intersects(rect))
            continue;
        QTransform textToPixels;
        if (!configureScreenshotOcrTextLayout(layout, line, canvasToPixels, snapshot.font,
                                              snapshot.textColor, {}, &textToPixels))
            continue;
        painter.save();
        painter.setTransform(textToPixels);
        layout.paint(&painter);
        painter.restore();
    }
    painter.end();
    return stopped() ? QImage{} : ScreenshotResultCompositor::compose(image, snapshot.resultStyle);
}
