#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONIMAGE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONIMAGE_H

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshottextannotationrenderer.h"
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
    ScreenshotTextAnnotationRenderSnapshot textAnnotations;
};

// Paints filtered pixels, recognized text and persistent marks, in that order, over an
// existing backdrop. The caller owns clipping, output opacity and result appearance.
// canvasToPainter maps into the painter's current logical coordinates (before its transform).
[[nodiscard]] bool paintScreenshotRecognitionOverlay(
    QPainter& painter, const ScreenshotRecognitionImageSnapshot& snapshot,
    const QTransform& canvasToPainter, const std::function<bool()>& cancelled = {});

[[nodiscard]] QImage
renderScreenshotRecognitionImage(const ScreenshotRecognitionImageSnapshot& snapshot,
                                 const std::function<bool()>& cancelled = {});

#endif
