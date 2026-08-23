#include "snow_shot/presentation/screenshotcanvascolorsampler.h"

#include <QColor>
#include <QImage>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QColor colorForPixel(int x, int y) {
    return QColor((x * 19 + y * 3) % 256, (x * 7 + y * 23) % 256, (x * 29 + y * 11) % 256);
}

void exactPhysicalPixelsRemainDistinct() {
    QImage raster(13, 11, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < raster.height(); ++y) {
        for (int x = 0; x < raster.width(); ++x) {
            raster.setPixelColor(x, y, colorForPixel(x, y));
        }
    }

    const QRect physicalBounds(-40, 75, raster.width(), raster.height());
    const QPoint firstPhysical = physicalBounds.topLeft() + QPoint(5, 4);
    const QPoint secondPhysical = firstPhysical + QPoint(1, 0);
    const QImage first = ScreenshotCanvasColorSampler::previewFromPhysicalRaster(
        raster, physicalBounds, firstPhysical);
    const QImage second = ScreenshotCanvasColorSampler::previewFromPhysicalRaster(
        raster, physicalBounds, secondPhysical);

    require(first.size() == QSize(ScreenshotCanvasColorSampler::PreviewSize,
                                  ScreenshotCanvasColorSampler::PreviewSize),
            "the canvas sampler preview must remain 7 by 7 physical pixels");
    require(first.pixelColor(3, 3) == colorForPixel(5, 4),
            "the preview center did not preserve the requested physical pixel");
    require(second.pixelColor(3, 3) == colorForPixel(6, 4) &&
                second.pixelColor(3, 3) != first.pixelColor(3, 3),
            "adjacent physical pixels collapsed to the same sampled color");
}

void fractionalLogicalCoordinatesMapBeforeRounding() {
    const QRect physicalBounds(500, 200, 150, 120);
    const QSize logicalSize(100, 80);
    const QPoint first = ScreenshotCanvasColorSampler::physicalPointForLocalPosition(
        QPointF(100.0 / 1.5, 45.0 / 1.5), logicalSize, physicalBounds);
    const QPoint second = ScreenshotCanvasColorSampler::physicalPointForLocalPosition(
        QPointF(101.0 / 1.5, 45.0 / 1.5), logicalSize, physicalBounds);

    require(first == QPoint(600, 245),
            "the first fractional logical position mapped to the wrong physical pixel");
    require(second == QPoint(601, 245),
            "the adjacent fractional logical position was rounded before physical mapping");
}

void previewClampsAtPhysicalEdges() {
    QImage raster(4, 3, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < raster.height(); ++y) {
        for (int x = 0; x < raster.width(); ++x) {
            raster.setPixelColor(x, y, colorForPixel(x, y));
        }
    }

    const QRect physicalBounds(10, 20, raster.width(), raster.height());
    const QImage preview = ScreenshotCanvasColorSampler::previewFromPhysicalRaster(
        raster, physicalBounds, physicalBounds.topLeft());
    require(preview.pixelColor(0, 0) == colorForPixel(0, 0) &&
                preview.pixelColor(3, 3) == colorForPixel(0, 0) &&
                preview.pixelColor(6, 6) == colorForPixel(3, 2),
            "the physical preview did not clamp consistently at the raster edge");
}
} // namespace

int main() {
    try {
        exactPhysicalPixelsRemainDistinct();
        fractionalLogicalCoordinatesMapBeforeRounding();
        previewClampsAtPhysicalEdges();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
