#pragma once

#include <QImage>
#include <QRectF>
#include <QTransform>
#include <functional>

class QPainter;

// Called synchronously after the backdrop and before document drawing. The transform maps
// canvas coordinates to the painter's output-pixel coordinates. False cancels the export.
using SnowCanvasExportBackgroundOverlay = std::function<bool(QPainter&, const QTransform&)>;

struct CanvasExportSource {
    QImage image;
    QRectF canvasRect;
    // Empty coverage exposes the entire source without allocating a cropped raster.
    QRectF coverage = {};
};

// Integer source pixels relative to the complete export frame, translated without scaling.
struct CanvasExportTile {
    QRect sourcePixels;
    QPoint destination;
};
