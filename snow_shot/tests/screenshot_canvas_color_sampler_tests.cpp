#include "snow_shot/presentation/screenshotcanvascolorsampler.h"

#include "snow_draw_engine_qt/snow_canvas_view.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include <QApplication>
#include <QPainter>
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
void sharedViewSamplingPreservesFractionalSurfacePixels() {
    class Renderer final : public SnowCanvasCustomRenderer {
      public:
        QImage image{QSize(1001, 31), QImage::Format_ARGB32_Premultiplied};
        Renderer() {
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    image.setPixelColor(x, y, colorForPixel(x, y));
        }
        void renderBeforeCanvas(QPainter& painter,
                                const SnowCanvasRenderContext& context) override {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.setTransform(context.canvasToViewTransform, true);
            painter.drawImage(QPointF(), image);
        }
    } renderer;
    for (qreal ratio : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        SnowCanvasView view;
        require(view.setSurfaceMetrics(renderer.image.size(), ratio),
                "set sampler surface metrics");
        view.setCanvasContentVisible(false);
        view.setClearBackgroundEnabled(false);
        view.setCustomRenderer(&renderer);
        require(
            view.setViewportCamera(view.width() * ratio / 2, view.height() * ratio / 2, 1 / ratio),
            "set pixel-aligned sampling camera");
        ScreenshotCanvasColorSampler sampler;
        const QRect bounds(QPoint(-1401, -73), renderer.image.size());
        require(sampler.ensureSnapshot(view, bounds),
                "sample shared canvas directly without a QWidget");
        for (const QPoint& pixel :
             {QPoint(0, 0), QPoint(499, 15), QPoint(500, 15), QPoint(999, 30), QPoint(1000, 30)}) {
            const QImage preview = sampler.previewAtPhysicalPoint(bounds.topLeft() + pixel);
            require(preview.pixelColor(3, 3) == colorForPixel(pixel.x(), pixel.y()),
                    "sampling must preserve exact source pixels through fractional view extents");
        }
        view.setCustomRenderer(nullptr);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        exactPhysicalPixelsRemainDistinct();
        fractionalLogicalCoordinatesMapBeforeRounding();
        previewClampsAtPhysicalEdges();
        sharedViewSamplingPreservesFractionalSurfacePixels();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
