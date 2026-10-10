#pragma once

#include <QImage>
#include <QRectF>

// Immutable original image pixels. Empty coverage exposes the entire canvasRect;
// a nonempty coverage limits a layer without allocating a cropped image.
struct SnowCanvasBaseImageSource {
    QImage image;
    QRectF canvasRect;
    QRectF coverage;
};
