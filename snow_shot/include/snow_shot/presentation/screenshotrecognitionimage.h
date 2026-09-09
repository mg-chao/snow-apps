#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONIMAGE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONIMAGE_H

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include <QFont>
#include <QImage>
#include <functional>

// Value snapshot: images and line containers use Qt copy-on-write; no session or widget references.
struct ScreenshotRecognitionImageSnapshot final {
    QImage image;
    QRectF canvasRect;
    QImage filteredImage;
    QRectF filteredCanvasRect;
    QVector<ScreenshotOcrLine> lines;
    QFont font;
    QColor textColor;
    ScreenshotResultStyle resultStyle;
};

[[nodiscard]] QImage
renderScreenshotRecognitionImage(const ScreenshotRecognitionImageSnapshot& snapshot,
                                 const std::function<bool()>& cancelled = {});

#endif
