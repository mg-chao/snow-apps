#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONIMAGE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONIMAGE_H

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include <QFont>
#include <QImage>
#include <functional>
#include "snow_draw_engine_qt/snow_canvas_export_types.h"

// Value snapshot: images and line containers use Qt copy-on-write; no session or widget references.
struct ScreenshotRecognitionImageSnapshot final {
    QImage image;
    QRectF canvasRect;
    QImage filteredImage;
    QRectF filteredCanvasRect;
    QVector<ScreenshotOcrLine> lines;
    QFont font;
    QColor textColor;
    // Always in canvasRect units. Rendering owns the sole conversion to image pixels.
    ScreenshotResultStyle resultStyle;
    ScreenshotLayoutCompaction layoutCompaction = {};
    qreal outputOpacity = 1.0;
    QList<CanvasExportSource> backgroundSources;
    std::optional<QRectF> visibleCanvasRect = std::nullopt;
};

[[nodiscard]] QImage
renderScreenshotRecognitionImage(const ScreenshotRecognitionImageSnapshot& snapshot,
                                 const std::function<bool()>& cancelled = {});

#endif
